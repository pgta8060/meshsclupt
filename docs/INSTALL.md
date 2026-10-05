# تثبيت Sculpt Mesh (المرحلة 1)

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

```maxscript
convertToSculpt $          -- يحوّل الأوبجكت المختار لـ Sculpt Mesh
SculptMesh.StartSculpt()   -- يدخل وضع النحت
SculptMesh.Brush = #smooth -- #sculpt | #smooth | #inflate | #pinch
SculptMesh.BrushSize = 60  -- نصف قطر الفرشاة بالبكسل
SculptMesh.BrushStrength = 0.4
SculptMesh.StopSculpt()
```

أو من الـ Modify panel: زرار **Sculpt** في rollout اسمه **Sculpt Mesh**.

- **سحب بالزرار الشمال:** نحت.
- **Shift:** Smooth مؤقت.
- **Alt:** عكس اتجاه الفرشاة.
- **كليك يمين أثناء الضربة:** إلغاء الضربة.
- **كليك يمين من غير ضربة:** خروج من وضع النحت.
- **Ctrl+Z:** بيرجّع ضربة كاملة.

> في المرحلة 1، العرض في الـ viewport بيبني الشبكة كلها من الأول بعد كل تحديث، فالأنسب تجرّب على شبكات لحد حوالي 250 ألف polygon. السرعة على الشبكات الأكبر هي شغل المرحلة 2.
