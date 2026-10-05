# خطة بناء بلاجن Sculpt Mesh لـ 3ds Max

الخطة دي بتقسم البلاجن لـ 12 مرحلة. كل مرحلة بتطلع حاجة **شغالة ومتختبرة**،
ومفيش مرحلة بتبدأ قبل ما اللي قبلها تعدّي "بوابة الجودة" (تحت).
الهدف إن البلاجن يكبر طبقة فوق طبقة على أساس متين، من غير ما نرجع نكسر حاجة اشتغلت.

---

## 1. المعمارية (ليه متقسم كده)

```
┌──────────────────────────────────────────────────────────────┐
│ 3ds Max                                                      │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ max/  (SculptMesh.dlo) — طبقة رفيعة بتكلم الـ SDK      │  │
│  │  • SculptMeshObject  (أوبجكت مشتق من PolyObject)       │  │
│  │  • convertToSculpt + MacroScript + واجهة SculptMesh    │  │
│  │  • Command Mode للماوس + Undo + رسم الفرشاة             │  │
│  │  • الواجهة العائمة + rollout الـ Multires + الاختصارات   │  │
│  └───────────────┬────────────────────────────────────────┘  │
│                  │ بيادي/بياخد positions و indices بس        │
│  ┌───────────────▼────────────────────────────────────────┐  │
│  │ core/  (SculptCore) — محرك النحت، C++17 خالص            │  │
│  │  • Mesh + adjacency + normals   • BVH (ray / sphere)    │  │
│  │  • الفرش + falloff + الضربات    • Undo deltas           │  │
│  │  ❖ مفيهوش ولا سطر من 3ds Max → بيتختبر أوتوماتيك       │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
```

**القرارات الأساسية:**

| القرار | السبب |
|---|---|
| المحرك (`core/`) منفصل تماماً عن 3ds Max | كل الرياضيات والفرش بتتختبر أوتوماتيك على أي جهاز وفي الـ CI، من غير ما نفتح Max. أغلب الأخطاء الخطيرة بتتمسك هنا. |
| أوبجكت `Sculpt Mesh` مشتق من `PolyObject` | بيورث العرض في Nitrous والـ render والـ snapping والتحويل لـ Editable Poly/Mesh والـ UVs والماتيريال IDs. ده كله كود Autodesk المختبر، فمش هنعيد كتابته. |
| C++ وليس MAXScript أو Python | النحت على مليون polygon محتاج أداء native. MAXScript موجود بس للأوامر والـ MacroScript. |
| صيغة الحفظ فيها رقم إصدار من أول يوم | أي ملف `.max` اتحفظ بنسخة قديمة من البلاجن لازم يفتح في الجديدة. |
| كل عملية بتتسجل في Undo | الـ undo/redo من أول مرحلة، مش حاجة نضيفها في الآخر. |
| بناء وفحص أوتوماتيك (CI) لكل push | Linux: اختبارات المحرك + فحص كود البلاجن على هيدرز الـ SDK الحقيقية. Windows: بناء حقيقي بـ MSVC لكل نسخة Max مدعومة. |

**النسخ المدعومة:** 3ds Max 2024 و 2025 و 2026 (Windows x64).

---

## 2. بوابة الجودة (لازم تتحقق قبل أي مرحلة جديدة)

1. **الاختبارات الآلية خضرا كلها:** اختبارات المحرك (بـ AddressSanitizer وUBSan)، وفحص الـ SDK، وبناء MSVC لكل نسخ Max.
2. **الاختبار اليدوي في 3ds Max:** قائمة المرحلة في `docs/testing/` بتتنفذ في Max، والنتيجة بتتكتب.
3. **مفيش crash** في أي سيناريو من القائمة، ومن ضمنها: Undo/Redo، Save/Load، مسح الأوبجكت وهو في وضع النحت، Reset/New/Open للمشهد.
4. **التوافق الرجعي:** ملفات المراحل اللي فاتت بتفتح من غير مشاكل.
5. **مفيش رجوع للخلف:** كل خاصية من مرحلة سابقة لسه شغالة.

---

## 3. المراحل

### المرحلة 1 — الأساس *(اتنفذت)*
**الهدف:** نثبت المعمارية من أولها لآخرها بأقل عدد خصائص، بس كلها متينة.

- محرك `SculptCore`: Mesh (polygons بأي عدد أضلاع) + adjacency + اكتشاف الحواف المفتوحة + normals كاملة وجزئية، و BVH (raycast + sphere query + refit جزئي)، و falloff، وضربة بمسافة ثابتة بين النقاط (spacing)، و Undo delta.
- 4 فرش: **Sculpt** و **Smooth** و **Inflate** و **Pinch**، مع Add/Sub و Alt للعكس و Shift لـ Smooth مؤقت.
- أوبجكت **Sculpt Mesh** في 3ds Max: حفظ وتحميل بصيغة فيها إصدار، و Clone، والتحويل لـ Editable Poly/Mesh.
- `convertToSculpt $` و MacroScript `Convert_To_Sculpt_Mesh` (category `MLtools`).
- واجهة MAXScript ثابتة اسمها `SculptMesh` (`SculptMesh.BrushSize` …) عشان الأتمتة والاختبار.
- وضع النحت في الـ viewport: الضربة، ودايرة الفرشاة، و **ضربة = خطوة Undo واحدة**، والكليك اليمين أو Esc بيلغي الضربة.
- Rollout مؤقت في الـ Modify panel (الواجهة العائمة جاية في المرحلة 3).
- الحماية: الخروج من وضع النحت أوتوماتيك عند تغيير الاختيار، أو مسح الأوبجكت، أو Reset/New/Open، أو تغيير التوبولوجي.
- CI كامل + أداة فحص الـ SDK على Linux + قائمة اختبار يدوي.

**القبول:** كل اختبارات المحرك خضرا، وكود البلاجن بيعدّي فحص هيدرز الـ SDK الحقيقية، والبلاجن بيتبني بـ MSVC، وقائمة `docs/testing/PHASE1.md` بتعدّي في Max.

### المرحلة 2 — الأداء والعرض في الـ Viewport *(اتنفذت)*
**الهدف:** النحت يبقى سلس على **1 إلى 4 مليون polygon**.

- Render items خاصة بـ Nitrous، بتحدّث بس أجزاء الـ vertex buffer اللي اتغيرت، بدل ما الشبكة كلها تتبني من الأول في كل dab.
- تقسيم الشبكة لمناطق (chunks) فيها dirty flags، وتحديث الـ normals والـ BVH جزئياً بس.
- الفرش تشتغل على أكتر من thread (parallel dabs).
- Undo بذاكرة قليلة (ضغط الـ deltas).
- Benchmarks في الـ CI بحدود أداء: لو الأداء وقع، الـ CI بيفشل.
- **القبول:** 1M polygon أكتر من 30 dab/ثانية على جهاز متوسط، ومفيش تقطيع في الرسم.
- **اللي اتعمل:** العرض بقى render items خاصة بـ Nitrous مقسومة chunks، ولون الماسك والجروبات جاي من texture صغيرة. الـ dab على مليون polygon بياخد حوالي 0.8 ms في الـ benchmark. Undo بيسجل اللي اتغير بس.

### المرحلة 3 — الواجهة العائمة *(اتنفذت)*
> **قرار:** الواجهة اتعملت بـ Win32 ورسم خاص بينا (owner-drawn) بألوان ثيم 3ds Max نفسه، بدل Qt. السبب إن Qt مختلف بين نسخ Max، ومعقد في البناء على الـ CI. والنتيجة نفس الشكل والسلوك، وبتتبني وتتفحص على 2024 و 2025 و 2026 بنفس الكود.
- نوافذ عائمة: شريط أدوات شمال (Select، و Sculpt/Paint، و Mask، و Stroke Mode، ومعاينة الـ Alpha)، و palette تحت (Sculpting / Alphas دلوقتي، و Paint / Meshes مع مراحلهم) بالسحب لإعادة الترتيب والتمرير بعجلة الماوس، و rollouts يمين.
- Rollout **Brush Settings** بكل أزراره، اللي بتظهر وتختفي حسب الفرشاة.
- **Quick Menu** (مسك Space أو كليك يمين)، والأرقام 1–5 لأول خمس خانات، وإدارة الـ keyboard focus.
- حفظ إعدادات كل فرشاة لوحدها بين الجلسات.
- زرار **Open/Close Sculpt Mesh Menus** في الـ Multires rollout.
- الاختصارات (1–5، و Space، و W/E/R، و Ctrl+W) في action table اسمه **Sculpt Mesh**، فاليوزر يقدر يغيّرها من الـ Hotkey Editor.

### المرحلة 4 — نظام الضربة والإدخال *(اتنفذت)*
- Stroke Spacing، و Follow Path، و Lazy Mouse، و Backface Cull لكل فرشاة.
- **Ctrl+Alt** للخط المستقيم، و **Ctrl+Shift** + سحب لتغيير الحجم.
- ضغط القلم (pressure) **اتأجل**: الـ mouse callback بتاع Max مبيديش الضغط، ومحتاج طريقة تانية (Windows Ink أو WinTab). المحرك جاهز وبيستقبل الضغط مع كل dab.
- Stroke Modes: **Draw و Stamp و Drag و Scatter** (و Color Mix في مرحلة الرسم).
- Alphas: الأربعة المدمجين، و Alpha Mid/Fade، ومكتبة Alphas بالتصنيفات والمفضلة.
- **Mirror:** X/Y/Z و Radial من 2 لـ 32.

### المرحلة 5 — فرش النحت الأساسية *(اتنفذت)*
Clay و Clay Buildup و Carve و Knife و Contrast و Scrape (Original Plane/Normal) و Polish (Hardness) و Move (AccuCurve، والمسك من بره السطح) و Snake Hook و **Layer Mode** لـ Sculpt.

### المرحلة 6 — الـ Mask و الـ SculptGroups *(اتنفذت)*
- Paint Mask و Rectangle و Lasso، وكل الـ gestures (Ctrl مؤقت، والكليك في الفراغ، والدبل كليك للـ blur).
- Blur و Sharpen و Grow و Shrink و Clear و Invert، و Mask by Cavity و Mask by AO.
- الـ Mask بيحمي الفرش، و W/E/R بيحركوا الجزء اللي مش متعمله mask.
- SculptGroups: **Ctrl+W**، و Auto Groups (Curvature، Angle، Smooth Groups، UV Islands، Material IDs، Elements)، وإظهار/إخفاء الجروبات، وفرشتين Face groups و Smooth SG Border.
- الماسك والجروبات والأجزاء المخفية بيتحفظوا في الملف (صيغة الحفظ رقم 2، والملفات القديمة بتفتح عادي).

> **اختبار المراحل 2–6 يدوياً:** [docs/testing/PHASES2-6.md](testing/PHASES2-6.md).

### المرحلة 7 — Multires و Surface Snapshot *(اتنفذت)*
- مستويات تقسيم لحد 6، والتنقل بينها مع الحفاظ على التفاصيل.
- Reverse Subdivision و Del Lower و Del Higher، و Use Materials ID / Smoothing Groups، و Autosmooth.
- حفظ مضغوط وسريع للمستويات.
- Surface Snapshot وفرشة **Revert**.

### المرحلة 8 — الطبقات و Displace *(اتنفذت)*
- Sculpt Layers: جديدة، ومسح، و Clear، وترتيب، و Bake All، و Strength لحد 5×.
- Rollout **Displace**: صورة، و UV أو Triplanar، و Strength، و Water Level، و Blur، و Contrast، و Tile/Offset، و Live Update.

### المرحلة 9 — الفرش المتخصصة *(اتنفذت)*
- Pose و Cloth (بكل إعداداته) و **Curve Tube** (مع Pick Section Shape) و rollout **Profile** (محرر curve Bezier).
- Clip و Cutter و Slice (مع Space للتحريك)، و Density/Reduce (الشبكة بتتعدل لما الضربة تخلص).
- فرشة Displace اتنقلت للمرحلة 10 عشان بتعتمد على الـ Stencil.
- العمليات اللي بتغيّر الـ topology (Density و Cutter و Slice و Curve Tube) بتشيل مستويات الـ Multires والطبقات (بتدمجها في الشكل)، وكلها خطوة undo واحدة.

### المرحلة 10 — الرسم (Texture Paint) *(اتنفذت)*
- Paint Source (Generated/Existing Diffuse)، و Color A/B، و Save/Save As/Replace/Restore.
- 6 أدوات: Paint و Smudge و Fill و Blur و Erase و Gradient، و Color Mix.
- Paint Layers بالـ blend modes (Normal و Multiply و Screen و Overlay و Add و Subtract)، و Hue/Sat/Lum و Brightness/Contrast و Levels، و Import Texture.
- Stencil: تحميل، و opacity، ومسك **S** للتدوير والتكبير والتحريك، وفرشة **Displace** (الـ Stencil كارتفاع).
- الرسم محتاج UVs في map channel 1. الطبقات بتفضل موجودة طول ما Max مفتوح؛ اللي بيتحفظ مع المشهد هو ملف الصورة (Save / Save As).

> **اختبار المراحل 7–10 يدوياً:** [docs/testing/PHASES7-10.md](testing/PHASES7-10.md).

### المرحلة 11 — عمليات الشبكة
- **Remesh:** Voxelize (من 2024 وأحدث) و Retopology (ReForm، و Ctrl لـ Instant Meshes).
- **Deformers/Tools:** Repeat Last Stroke، و Close Holes، و Extract/Attach/Detach، و Smooth Surface، و Thickness، و Gravity، و FOV، و Bevel، و Projection.

### المرحلة 12 — Mesh Library و Bake Maps والتسليم
- Mesh Library: مكتبة وتصنيفات ومفضلة، و Add Selected، والتركيب على السطح بـ Boolean (Subtract و Union و Insert)، والـ thumbnails.
- **Bake Maps:** Auto Unwrap، و Normal، و Displacement EXR، و Curvature، و AO، و Padding.
- History في MAXScript (`SculptMesh.History` …)، والترخيص، وملف التثبيت (installer).

---

## 4. ترتيب المراحل ده ليه؟

- **الأداء (2) قبل أي فرش جديدة:** لو بنينا فرش كتير على مسار عرض بطيء، هنضطر نعدّل فيها كلها بعدين.
- **الواجهة (3) بدري:** كل خاصية بعد كده بتدخل في مكانها النهائي، ومش محتاجين واجهات مؤقتة.
- **Multires (7) بعد الـ Mask:** الـ Multires بيغيّر طريقة تخزين البيانات، فلازم الـ Mask والـ Groups يبقوا موجودين عشان نصممه يشيلهم من الأول.
- **العمليات اللي بتغيّر التوبولوجي (9 و 11 و 12) في الآخر:** لأنها بتأثر على الـ layers والـ masks والـ multires، فلازم يكونوا كلهم ثابتين قبلها.

## 5. المخاطر وإزاي بنتعامل معاها

| الخطر | إزاي بنتعامل معاه |
|---|---|
| ملناش 3ds Max على سيرفر الـ CI | المحرك متختبر بالكامل، وكود البلاجن بيتفحص على هيدرز الـ SDK الحقيقية وبيتبني بـ MSVC، والاختبار اليدوي في Max شرط في بوابة الجودة. |
| الأداء على الشبكات الكبيرة | المرحلة 2 كلها للأداء، والـ benchmarks بقت جزء من الـ CI. |
| فرق الـ API بين نسخ Max | الـ CI بيبني على 2024 و 2025 و 2026، والكود اللي بيعتمد على نسخة معينة معزول. |
| فقدان الشغل (crash أو ملف بايظ) | صيغة حفظ فيها إصدار، و Undo لكل حاجة، والخروج الآمن من وضع النحت مع أحداث المشهد. |
