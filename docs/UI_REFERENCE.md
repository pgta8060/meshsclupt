# مرجع الواجهة من الصور (بيتنفذ من المرحلة 3 وبعدها)

الملف ده بيوصف الواجهة اللي في الصور اللي بعتّها، عشان المراحل الجاية تبني نفس الشكل بالظبط.
كل الـ rollouts بثيم غامق، وكل واحد له عنوان بسهم ▾/▸، وكل قيمة رقمية ليها slider وجنبه خانة رقم.

## التقسيم العام (الـ workspace الأساسي)
- **شريط أدوات رأسي على الشمال:** Select، ثم قايمة Sculpt/Paint (Sculpt و Paint)، ثم قايمة Mask، ثم قايمة Stroke Mode، ثم معاينة الـ alpha الحالي (أيقونة صورة).
- **الـ palette اللي تحت:** تابات `Sculpting | Paint | Alphas` (و `Meshes` بعدين)، وفيها أيقونات الفرش المدورة واسم كل فرشة تحتها.
  ترتيب الفرش في الصورة: Sculpt، Clay Buildup، Move، Crease، Polish، Scrape، Smooth، Clay، Clip، Bulge، Snake Hook، Cloth، Pose، Curve Tube، Knife، Slice، Flatten، Pinch، Inflate، Smooth SG Border، Trim…
- **rollouts عائمة على اليمين:** Brush Settings، و Material / Paint، و Mask، و Mirror، و Profile، و Surface Snapshot، و Layers.
- **الـ Modify panel:** في أول صور كان فيه `MeshSculpt` في الـ stack وتحته rollout اسمه **Parameters**. في آخر صور الاسم بقى **Sculpt Mesh**، والـ rollouts بقت **Multires** و **Deformers / Tools** و **Remesh** (التفاصيل تحت). المعتمد هو آخر صور.
- **دايرة الفرشاة:** حمرا، ودايرتين جوه بعض (الحافة الخارجية ودايرة داخلية).

## Brush Settings
- زرارين `Add` | `Sub` (الشغال فيهم بيكون منوّر).
- Brush Size (slider + 12.5)، و Brush Strength (0.153)، و ☑ Layer Mode، و Stroke Spacing (0.18)، و Alpha Mid (0.01)، و Alpha Fade (0.00).
- ☐ Use Falloff، و ☑ Follow Path، و ☑ Lazy Mouse مع slider (0.25)، و ☑ Backface Cull.

## Material / Paint
- ☑ Use Sculpt Material Preview.
- Paint Source: `Generated Texture` ▾ وجنبها الدقة `2048` ▾.
- Color A (مربع أبيض)، ثم زرار ⇄ للتبديل، ثم Color B (مربع أسود).
- `Save` | `Save As`، و `Replace Texture` | `Restore Material`.
- `Load Stencil`، وجنبه slider للشفافية (50%)، وزرار reset، وزرار مسح.

## Mask
- `Mask by Cavity` مع slider (0.55)، و `Mask by AO` مع slider (0.55).
- `Blur Mask` | `Sharpen Mask`، و `Grow Mask` | `Shrink Mask`، و `Clear Mask` | `Invert Mask`.
- ☑ Show Mask، و ☐ Show SculptGroups.
- `Auto Groups` وجنبه قايمة: Curvature، و Angle، و Smooth Groups، و UV Islands، و Material IDs، و Elements.

## Profile
- ☑ Use Profile، و Apply to: `Active Sculpt Group` ▾، و Mapping: `Local Z` ▾.
- محرر curve فيه grid ونقط Bezier بالـ handles بتاعتها، وتحته زرار `Reset Profile`.

## Layers
- تابات: `Sculpt` | `Paint`.
- صف أزرار: New، و Delete، و Clear، و Clear All، و Move Up، و Move Down، و Bake All.
- لستة الـ layers (اللي متختار بيكون منوّر)، وتحتها خانة الاسم، و ☑ Layer Enabled، و slider للـ Strength (النص = 1×).
- **تاب Paint:** جنب الأزرار قايمة blend mode (Normal، و Multiply، و Screen، و Overlay، و Add، و Subtract)، وتحت اللستة 3 أزرار للتعديلات (Hue/Sat/Lum، و Brightness/Contrast، و Levels).

## صور تانية
- الـ Alphas palette: الأول No Alpha، بعده 4 alphas مدمجين (ناعم وحاد ومربع)، وبعدين زخارف نباتية (المكتبة الخاصة).
- مثال Cutter: كرة فيها قطع دايري نضيف، وحواف القطع متقفلة بشريط مثلثات.

## آخر مجموعة صور (الماسك، والـ Quick Menu، والـ Modify panel)
- **Mask flyout:** أيقونات Paint Mask و Rectangle و Lasso، والأداة المختارة منوّرة.
- **Quick Menu:** مربع ألوان (Saturation / Value) وجنبه شريط Hue رأسي، وبعده Color A وزرار ⇄ و Color B. وتحتهم Brush Size (12.50) و Brush Strength (1.00)، وبعدين ☐ Use Alpha Texture، و ☑ Use Falloff، و ☑ Follow Path، و ☑ Backface Cull.
- **Multires:** زرار `Open Sculpt Mesh Menus` / `Close Sculpt Mesh Menus`، وبعده باقي أدوات المستويات (المرحلة 7).
- **Deformers / Tools** (المرحلة 11): `Repeat Last Stroke` وجنبه رقم (1)، و `Close Holes` | `Extract`، و `Attach` | `Detach`، و sliders لـ Smooth Surface و Thickness و Gravity و FOV، و `Bevel Round` | `Chamfer`، و Projection: `Pick Mesh` وجنبه رقم (3.00).
- **Remesh** (المرحلة 11): `Voxelize Mesh` وجنبه الدقة (256)، و `Retopology` وجنبه عدد الـ polygons (2000).
- **خانة الـ Alpha في الشريط الشمال:** بتعرض الـ alpha المختار، ولو مفيش بتقول إن مفيش alpha.

## اللي اتنفذ لحد المرحلة 10
- الشريط الشمال: Select، و Sculpt/Paint، و Mask، و Stroke Mode (Draw و Stamp و Drag و Color Mix و Scatter)، والـ Alpha.
- الـ palette: تابات **Sculpting** و **Paint** و **Alphas** (السحب لإعادة الترتيب شغال في Sculpting و Paint، والأرقام 1–5 بتمشي مع الوضع الحالي). تاب Meshes هييجي مع المرحلة 12.
- الـ rollouts العائمة: **Brush Settings** (بتتغير مع الفرشة أو أداة الرسم)، و **Material / Paint**، و **Mask**، و **Mirror**، و **Profile** (فيه محرر الـ curve)، و **Layers** (تابين Sculpt و Paint)، و **Surface Snapshot**.
- الـ Quick Menu زي الصورة بالظبط، و Color A / Color B بيتستخدموا في الرسم.
- الـ Modify panel: rollout اسمه **Multires** (Open/Close Sculpt Mesh Menus، و Reverse Subdivision، و Del Lower/Del Higher، و Level، و Subdivide Level، و Materials ID، و Smoothing Groups، و Autosmooth)، و rollout اسمه **Displace**.
- الواجهة كلها مرسومة بألوان ثيم 3ds Max، فبتبقى غامقة مع الثيم الغامق وفاتحة مع الفاتح.
