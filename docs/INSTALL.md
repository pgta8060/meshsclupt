# تثبيت Sculpt Mesh

## من الـ CI (الأسهل)

1. افتح تبويب **Actions** في الريبو، واختار آخر تشغيل ناجح لـ **CI**.
2. نزّل الملف `SculptMesh-3dsMax2025` (أو النسخة اللي عندك: 2024 / 2025 / 2026).
3. اقفل 3ds Max.
4. انسخ `plugins/SculptMesh.dlo` لفولدر البلاجنز، مثلاً:
   `C:\Program Files\Autodesk\3ds Max 2025\Plugins\`
5. انسخ `macroscripts/SculptMesh_Macros.mcr` لـ:
   `C:\Program Files\Autodesk\3ds Max 2025\MacroScripts\`
   أو للفولدر بتاع اليوزر: `%LOCALAPPDATA%\Autodesk\3dsMax\2025 - 64bit\ENU\usermacros\`
6. افتح 3ds Max. لو البلاجن اتحمّل صح، الأمر ده في الـ MAXScript Listener بيرجّع رقم النسخة:
   ```maxscript
   SculptMesh.Version
   ```

## البناء من السورس (Windows)

المطلوب: Visual Studio 2022 (C++)، و CMake 3.20 أو أحدث، و 3ds Max SDK بنفس نسخة الـ Max.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DMAXSDK_DIR="C:\Program Files\Autodesk\3ds Max 2025 SDK\maxsdk"
cmake --build build --config Release
ctest --test-dir build -C Release
```

لو حابب البلاجن يتنسخ أوتوماتيك بعد كل build، زوّد للأمر الأول:
`-DSCULPT_MAX_PLUGIN_DIR="C:\Program Files\Autodesk\3ds Max 2025\Plugins"`

> لـ 3ds Max 2024 استخدم `-T v142`.

## الاستخدام السريع

1. اختار أوبجكت polygon، وحوّله بـ `convertToSculpt $` أو بالـ MacroScript **Convert to Sculpt Mesh** (category `MLtools`).
2. افتح الـ Modify panel. القوائم العائمة بتظهر فوق الـ viewports. ولو مقفولة، دوس **Open Sculpt Mesh Menus** في rollout اسمه **Multires**.
3. اختار فرشة من الـ palette اللي تحت، وابدأ انحت.
4. خلّي الـ viewport على **Standard** مش **High Quality**.

| الإدخال | الوظيفة |
|---|---|
| سحب بالزرار الشمال | نحت |
| Alt | عكس اتجاه الفرشاة |
| Shift | Smooth مؤقت (و Face groups: تكبير الجروب اللي تحت الماوس) |
| Ctrl | آخر أداة Mask اخترتها (و Ctrl+Alt: مسح الماسك) |
| Ctrl+Alt+كليك | خط مستقيم من آخر ضربة |
| Ctrl+Shift + سحب بالعرض | تغيير حجم الفرشاة |
| Ctrl+Shift+Alt+كليك | عزل جروب أو إخفاؤه؛ وفي الفراغ: إظهار الكل |
| 1 – 5 | أول 5 فرش في الـ palette |
| مسك Space، أو كليك يمين | Quick Menu |
| W / E / R | تحريك، أو تدوير، أو تكبير الجزء اللي مش عليه ماسك |
| Ctrl+W | SculptGroup من الماسك |
| كليك يمين أثناء الضربة، أو Esc | إلغاء الضربة |
| Ctrl+Z | Undo لضربة أو عملية كاملة |

**لو الاختصارات مش شغالة:** شغّل زرار **Keyboard Shortcut Override Toggle** في شريط Max، أو افتح **Customize > Hotkey Editor** وابحث عن group اسمه **Sculpt Mesh** وغيّر المفاتيح زي ما تحب. الاختصارات شغالة بس والـ Sculpt Mesh مفتوح في الـ Modify panel، وبعد ما تكليك في الـ viewport مرة.

**الإعدادات** (حجم الفرشاة، وقوة كل فرشة، وترتيب الـ palette، والمكتبة، والمفضلة) بتتحفظ لوحدها في `plugcfg\SculptMesh.ini`. ولو عايز ترجع للإعدادات الأصلية: `SculptMesh.ResetSettings()`.

### MAXScript

```maxscript
convertToSculpt $                 -- يحوّل الأوبجكت المختار لـ Sculpt Mesh
SculptMesh.StartSculpt()          -- يدخل وضع النحت
SculptMesh.Brush = #clay          -- #sculpt #clay #clayBuildup #carve #knife #contrast #scrape #polish
                                  -- #move #snakeHook #pinch #inflate #smooth #faceGroups #smoothGroupBorder
SculptMesh.BrushSize = 60         -- نصف القطر بالبكسل
SculptMesh.BrushStrength = 0.4
SculptMesh.SetValue "mirrorX" 1   -- أي إعداد بالاسم (موجودين في SculptMesh.ini)
SculptMesh.GetValue "strokeMode"  -- 0 Draw، 1 Stamp، 2 Drag، 4 Scatter
SculptMesh.Run "maskInvert"       -- maskClear maskInvert maskBlur maskSharpen maskGrow maskShrink
                                  -- maskByCavity maskByAO groupFromMask autoGroups showAll invertVisibility
SculptMesh.MenusOpen = true       -- يفتح القوائم العائمة
SculptMesh.StopSculpt()
```
