#include "ConvertToSculpt.h"

#include <modstack.h>
#include <maxscript/maxscript.h>
#include <maxscript/maxwrapper/mxsobjects.h>

#include "SculptMeshObject.h"

namespace {

bool Fail(MSTR* error, int stringId) {
    if (error) *error = GetString(stringId);
    return false;
}

}  // namespace

bool ConvertNodeToSculpt(INode* node, TimeValue t, MSTR* error) {
    if (!node) return Fail(error, IDS_ERR_CANNOT_CONVERT);
    Object* top = node->GetObjectRef();
    if (!top) return Fail(error, IDS_ERR_CANNOT_CONVERT);
    if (top->ClassID() == SCULPTMESH_CLASS_ID) return true;  // Already a Sculpt Mesh, no modifiers.

    // Keep the old pipeline alive while we read from it.
    TypedSingleRefMaker<Object> oldObject{top};
    const ObjectState os = oldObject->Eval(t);
    Object* evaluated = os.obj;
    if (!evaluated || evaluated->SuperClassID() != GEOMOBJECT_CLASS_ID ||
        !evaluated->CanConvertToType(polyObjectClassID))
        return Fail(error, IDS_ERR_CANNOT_CONVERT);

    SculptMeshObject* sculpt = nullptr;
    {
        // Building the new object is not an undo step of its own; only the
        // reference swap below is.
        HoldSuspend suspend;
        Object* converted = evaluated->ConvertToType(t, polyObjectClassID);
        if (!converted) return Fail(error, IDS_ERR_CANNOT_CONVERT);
        PolyObject* poly = static_cast<PolyObject*>(converted);

        sculpt = new SculptMeshObject();
        sculpt->mm = poly->mm;
        sculpt->InitFromPoly(*poly);
        if (converted != evaluated) converted->DeleteThis();  // Temporary made by ConvertToType.

        sculpt->mm.CollapseDeadStructs();
        if (!sculpt->mm.GetFlag(MN_MESH_FILLED_IN)) sculpt->mm.FillInMesh();
        // Locked/explicit normals (common in imported meshes) would not follow
        // the sculpted surface; let 3ds Max compute them from the geometry.
        if (sculpt->mm.GetSpecifiedNormals()) sculpt->mm.ClearSpecifiedNormals();
        if (sculpt->mm.numf <= 0) {
            sculpt->DeleteThis();
            return Fail(error, IDS_ERR_CANNOT_CONVERT);
        }
    }

    {
        NotifyCollapseEnumProc preCollapse(true, node);
        EnumGeomPipeline(&preCollapse, oldObject);
    }

    theHold.Begin();
    node->SetObjectRef(sculpt);
    GetCOREInterface7()->InvalidateObCache(node);
    node->NotifyDependents(FOREVER, 0, REFMSG_SUBANIM_STRUCTURE_CHANGED);
    theHold.Accept(GetString(IDS_UNDO_CONVERT));

    {
        NotifyCollapseEnumProc postCollapse(false, node, sculpt);
        EnumGeomPipeline(&postCollapse, oldObject);
    }
    return true;
}

// --- MAXScript: convertToSculpt <node | collection> ------------------------------
// Mapped: given a collection (e.g. `$` or `selection`) it runs per node.
// Returns the node on success, `undefined` for nodes that cannot be converted.

#include <maxscript/macros/define_instantiation_functions.h>

def_mapped_primitive(convertToSculpt, "convertToSculpt");

Value* convertToSculpt_cf(Value** arg_list, int count) {
    check_arg_count(convertToSculpt, 1, count);
    INode* node = arg_list[0]->to_node();
    MSTR error;
    if (!ConvertNodeToSculpt(node, MAXScript_time(), &error)) return &undefined;
    needs_redraw_set();
    return arg_list[0];
}
