// SculptCore — Pose and Cloth deformations, the Profile curve and Curve Tube
// geometry.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "sculpt/math.h"
#include "sculpt/mesh.h"
#include "sculpt/poly.h"

namespace sculpt {

// Rotation of `angle` radians about the unit `axis` (Rodrigues).
Mat3 axisAngle(const Vec3& axis, float angle);

// --- Pose -------------------------------------------------------------------------

enum class PoseDeformation : int { Rotate = 0, Twist = 1, Scale = 2 };

struct PoseSettings {
    PoseDeformation deformation = PoseDeformation::Rotate;
    float originOffset = 0.0f;  // Moves the pivot along pivot->handle (fraction of its length).
    int smoothIterations = 2;   // Softens the weight border.
    int ikSegments = 1;         // The rotation is spread over this many pivots along the guide.
    bool connectedOnly = true;  // Only geometry connected to the handle.
};

// Weight (0..1) of every vertex: 1 on the handle side of the plane through
// `pivot` facing `handle`, blended over `band` around the plane.
void poseWeights(const Mesh& mesh, const Vec3& pivot, const Vec3& handle, float band, const PoseSettings& settings,
                 std::vector<float>& weights);

// Rotation Origins = SculptGroups: weight 1 on the polygons of the group of
// `seedFace` that are connected to it, softened over the border; `pivot`
// becomes the centre of the border with the other groups. Returns false if
// the group has no border.
bool poseGroupWeights(const Mesh& mesh, const std::vector<std::int32_t>& faceGroups, std::uint32_t seedFace,
                      int smoothIterations, std::vector<float>& weights, Vec3& pivot);

// Posed positions for vertices `vertices` (original positions `original`):
// Rotate: the guide turns from pivot->handle to pivot->target.
// Twist: rotation about the guide by `amount` radians. Scale: by (1 + amount).
void posePositions(const std::vector<std::uint32_t>& vertices, const std::vector<Vec3>& original,
                   const std::vector<float>& weights, const Vec3& pivot, const Vec3& handle, const Vec3& target,
                   float amount, const PoseSettings& settings, std::vector<Vec3>& out);

// --- Cloth ------------------------------------------------------------------------------

struct ClothSettings {
    int iterations = 4;           // Constraint iterations per dab.
    float damping = 0.3f;         // 0..1 velocity loss.
    float plasticity = 0.0f;      // 0..1: rest shape follows the deformation.
    float bendiness = 0.5f;       // 0..1: how easily it bends (lower = stiffer cloth).
    float foldSize = 0.5f;        // 0..1 of the brush radius.
    float foldStrength = 0.0f;    // 0..1: compression that makes folds.
    float bendStiffness = 0.2f;   // 0..1 second-neighbour constraints.
    float simulationArea = 2.0f;  // Simulated radius in brush radii (1..5).
    float moveStrength = 1.0f;    // 0..1 of the mouse motion applied under the brush.
    float gravity = 0.0f;         // -1..1 toward object -Z.
    float pressure = 0.0f;        // -1..1 inflate along normals.
    bool pinBoundary = true;      // The simulation border stays fixed.
};

class ClothSim {
public:
    // Builds particles and constraints around `center`. Returns false if nothing is there.
    bool begin(const Mesh& mesh, const Vec3& center, float brushRadius, const ClothSettings& settings);
    bool active() const { return !particles_.empty(); }
    // One dab: drags the brush area by `grab`, `shrink` contracts it (Alt), then relaxes.
    void step(const Vec3& brushCenter, float brushRadius, const Vec3& grab, bool shrink,
              std::vector<std::uint32_t>& vertices, std::vector<Vec3>& targets);
    void end() { *this = ClothSim(); }

private:
    struct Constraint {
        std::uint32_t a, b;
        float rest;
        float stiffness;
    };
    ClothSettings settings_;
    std::vector<std::uint32_t> particles_;  // Mesh vertex of each particle.
    std::vector<Vec3> x_, prev_, normal_;
    std::vector<float> invMass_;
    std::vector<Constraint> constraints_;
    float radius_ = 1.0f;
};

// --- Profile curve -------------------------------------------------------------------------

enum class ProfilePointType : int { Bezier = 0, BezierCorner = 1, Smooth = 2, Linear = 3 };

struct ProfilePoint {
    float x = 0.0f, y = 1.0f;
    float inX = 0.0f, inY = 0.0f;    // Handle toward the previous point (offset).
    float outX = 0.0f, outY = 0.0f;  // Handle toward the next point (offset).
    ProfilePointType type = ProfilePointType::Smooth;
};

class ProfileCurve {
public:
    ProfileCurve();  // Flat at 1 from x = 0 to x = 1.
    std::vector<ProfilePoint>& points() { return points_; }
    const std::vector<ProfilePoint>& points() const { return points_; }
    // y at x in [0,1] (x clamped). Points are kept sorted by x.
    float evaluate(float x) const;
    // Recomputes Smooth handles and keeps every handle inside its segment.
    void normalize();
    int addPoint(float x, float y);
    void removePoint(int index);  // First and last points stay.
    std::string toText() const;
    bool fromText(const std::string& text);

private:
    std::vector<ProfilePoint> points_;
};

// --- Curve Tube --------------------------------------------------------------------------

struct TubeSettings {
    int sides = 12;
    float radius = 1.0f;
    float taperStart = 1.0f;  // Radius scale at the start / end (Ctrl+Alt drag).
    float taperEnd = 1.0f;
    const ProfileCurve* profile = nullptr;           // Radius along the length.
    const std::vector<float>* profileParam = nullptr;  // Optional profile x per curve point (else the length).
    const std::vector<std::array<float, 2>>* section = nullptr;  // Closed cross-section (unit size), optional.
};

// Smooth curve through the control points (Catmull-Rom), about `spacing` apart.
std::vector<Vec3> sampleCurve(const std::vector<Vec3>& controls, float spacing);

// Appends a closed tube along `curve` as a separate element of `poly`
// (quads around, capped ends; SculptGroup `group`, UVs on channel 1).
bool appendTube(PolyData& poly, const std::vector<Vec3>& curve, const TubeSettings& settings, std::int32_t group,
                std::string* error = nullptr);

// Profile applied to a SculptGroup: every vertex of `group` is scaled away
// from the group's axis (Local X/Y/Z = axis 0/1/2) by profile(t), t running
// along that axis over the group's extent.
void groupProfileTargets(const Mesh& mesh, const std::vector<std::int32_t>& faceGroups, std::int32_t group, int axis,
                         const ProfileCurve& profile, std::vector<std::uint32_t>& vertices, std::vector<Vec3>& targets);

}  // namespace sculpt
