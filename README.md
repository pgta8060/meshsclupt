# Sculpt Mesh — بلاجن نحت لـ 3ds Max

نحت، ورسم textures، و masks، و SculptGroups، و layers، و Multires، و remesh، و baking، كله جوه 3ds Max 2024 / 2025 / 2026.
البلاجن بيتبني على مراحل، وبين كل مرحلة والتانية بوابة جودة. شوف [الخطة (ROADMAP)](docs/ROADMAP.md).

**الحالة دلوقتي: المرحلة 1 (الأساس).** أوبجكت Sculpt Mesh، و `convertToSculpt`، و 4 فرش (Sculpt / Smooth / Inflate / Pinch)، و Undo لكل ضربة، وحفظ وتحميل.

| | |
|---|---|
| التثبيت والاستخدام | [docs/INSTALL.md](docs/INSTALL.md) |
| قائمة الاختبار اليدوي للمرحلة 1 | [docs/testing/PHASE1.md](docs/testing/PHASE1.md) |
| مرجع الواجهة المطلوبة | [docs/UI_REFERENCE.md](docs/UI_REFERENCE.md) |

## تنظيم الريبو

```
core/              SculptCore — محرك النحت، C++17 خالص (مفيهوش أي حاجة من 3ds Max)
  include/sculpt/  mesh, bvh, brush, stroke, session
  tests/           اختبارات الوحدة (بتشتغل على أي جهاز)
  bench/           قياس الأداء
max/               بلاجن 3ds Max (SculptMesh.dlo) — طبقة رفيعة فوق الـ SDK
  src/             الأوبجكت، والتحويل، ووضع النحت، والـ panel، و MAXScript
  scripts/         الـ MacroScripts
  tests/           اختبارات لمنطق البلاجن اللي مش محتاج الـ SDK
tools/maxsdk_check أداة بتفحص max/src على هيدرز 3ds Max SDK الحقيقية من Linux
docs/              الخطة، والتثبيت، وقوايم الاختبار
```

## البناء والاختبار

المحرك والاختبارات (Linux / macOS / Windows):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSCULPT_SANITIZE=address,undefined   # الـ sanitizers مع GCC/Clang بس
cmake --build build
ctest --test-dir build --output-on-failure
./build/core/sculpt_bench 1000 1000      # حوالي 2 مليون مثلث
```

البلاجن نفسه (Windows + Visual Studio 2022): شوف [docs/INSTALL.md](docs/INSTALL.md).

فحص كود البلاجن على الـ SDK من Linux (بينزّل الـ SDK لوحده):

```sh
sudo apt install clang llvm mingw-w64 msitools
tools/maxsdk_check/check.sh --sdk-version 2025
```

## الـ CI

مع كل push بيحصل الآتي:
1. اختبارات المحرك بـ AddressSanitizer و UBSan على GCC و Clang.
2. فحص كود البلاجن على هيدرز SDK لـ 3ds Max 2024 و 2025 و 2026.
3. بناء حقيقي للبلاجن بـ MSVC لكل نسخة، ورفعه كـ artifact جاهز للتثبيت.
