#!/usr/bin/env bash
# Checks the 3ds Max plugin sources (max/src) against the real 3ds Max SDK
# headers without Windows or MSVC: clang parses every file with MSVC
# compatibility on top of the mingw-w64 Windows headers.
#
# This catches wrong SDK API usage (unknown methods, wrong signatures, missing
# overrides, typos) early. It does not replace the MSVC build in CI, which is
# the authority on whether the plugin compiles and links.
#
# Usage:
#   tools/maxsdk_check/check.sh [--sdk-version 2025] [--sdk-dir /path/to/maxsdk] [files...]
#
# Requirements: clang++ (>= 14), mingw-w64 headers (x86_64-w64-mingw32),
#               python3; msitools (msiextract) and curl when downloading the SDK.
# The SDK is downloaded once into $MAXSDK_CACHE (default ~/.cache/sculptmesh/maxsdk).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/tools/maxsdk_check"
VERSION="2025"
SDK_DIR=""
FILES=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sdk-version) VERSION="$2"; shift 2 ;;
    --sdk-dir) SDK_DIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) FILES+=("$1"); shift ;;
  esac
done

CXX="${CXX:-clang++}"
command -v "$CXX" >/dev/null || { echo "error: $CXX not found" >&2; exit 2; }
MINGW_INC="${MINGW_INC:-/usr/x86_64-w64-mingw32/include}"
[[ -f "$MINGW_INC/windows.h" ]] || { echo "error: mingw-w64 headers not found at $MINGW_INC (apt install mingw-w64)" >&2; exit 2; }

# --- Locate or download the SDK ------------------------------------------------
if [[ -z "$SDK_DIR" ]]; then
  CACHE="${MAXSDK_CACHE:-$HOME/.cache/sculptmesh/maxsdk}/$VERSION"
  SDK_DIR="$CACHE/maxsdk"
  if [[ ! -f "$SDK_DIR/include/max.h" ]]; then
    command -v msiextract >/dev/null || { echo "error: msiextract not found (apt install msitools)" >&2; exit 2; }
    mkdir -p "$CACHE"
    URL="https://autodesk-adn-transfer.s3-us-west-2.amazonaws.com/ADN+Extranet/M%26E/Max/Autodesk+3ds+Max+${VERSION}/SDK_3dsMax${VERSION}.msi"
    echo "Downloading 3ds Max $VERSION SDK..."
    curl -fsSL --retry 4 -o "$CACHE/sdk.msi" "$URL"
    rm -rf "$CACHE/extract"
    mkdir -p "$CACHE/extract"
    msiextract -C "$CACHE/extract" "$CACHE/sdk.msi" >/dev/null
    found="$(find "$CACHE/extract" -type f -path '*/maxsdk/include/max.h' | head -n 1)"
    [[ -n "$found" ]] || { echo "error: max.h not found in the extracted SDK" >&2; exit 2; }
    rm -rf "$SDK_DIR"
    mv "$(dirname "$(dirname "$found")")" "$SDK_DIR"
    rm -rf "$CACHE/extract" "$CACHE/sdk.msi"
  fi
fi
[[ -f "$SDK_DIR/include/max.h" ]] || { echo "error: no SDK at $SDK_DIR" >&2; exit 2; }

# --- Generated helpers ------------------------------------------------------------
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp "$HERE/shim/msvc_shim.h" "$WORK/msvc_shim.h"
# clang in MS-compatibility mode drops the GCC builtin macros libstdc++ needs.
echo | "$CXX" --target=x86_64-w64-windows-gnu -dM -E -x c++ - |
  grep -E '#define __(GCC_|GXX_|ATOMIC_)' |
  awk '{name=$2; $1=""; $2=""; sub(/^  /, ""); printf "#ifndef %s\n#define %s %s\n#endif\n", name, name, $0}' \
  >"$WORK/gnu_macros.h"
python3 "$HERE/make_overlay.py" "$WORK/overlay.yaml" "$SDK_DIR/include" "$MINGW_INC" "$ROOT/max/src"

FLAGS=(
  --target=x86_64-w64-windows-gnu -std=c++17 -fsyntax-only
  -fms-extensions -fms-compatibility -fms-compatibility-version=19.38
  -fdelayed-template-parsing -fdeclspec
  -D__GNUC__=4 -D__GNUC_MINOR__=2 -D__GNUC_PATCHLEVEL__=1
  -D_MSC_VER=1938 -D_MSC_FULL_VER=193833130 -D_CPPRTTI -DNO_INIUTIL_USING
  -DUNICODE -D_UNICODE -DWIN32 -DWIN64 -D_WIN64 -D_WINDOWS -DWIN32_LEAN_AND_MEAN -DNOMINMAX
  -D_ADESK_3DSMAX_WINDOWS_ -DMODULE_NAME=SculptMesh.dlo
  -ivfsoverlay "$WORK/overlay.yaml"
  -include "$WORK/msvc_shim.h"
  -I "$ROOT/max/src" -I "$ROOT/core/include" -isystem "$SDK_DIR/include"
  # SDK headers are not ours: only report errors; warnings for our code below.
  -Wno-everything
  -Werror=return-type -Werror=uninitialized -Werror=delete-non-abstract-non-virtual-dtor
  -Werror=overloaded-virtual -Werror=inconsistent-missing-override -Werror=suggest-override
  -ferror-limit=50
)

if [[ ${#FILES[@]} -eq 0 ]]; then
  mapfile -t FILES < <(find "$ROOT/max/src" -name '*.cpp' | sort)
fi

status=0
for f in "${FILES[@]}"; do
  if "$CXX" "${FLAGS[@]}" "$f"; then
    echo "ok    ${f#"$ROOT"/}"
  else
    echo "FAIL  ${f#"$ROOT"/}"
    status=1
  fi
done

# The resource script must compile too (llvm-rc understands the same syntax as rc.exe).
RC="${LLVM_RC:-$(command -v llvm-rc || true)}"
if [[ -n "$RC" ]]; then
  if "$RC" /nologo /I "$ROOT/max/src" /I "$MINGW_INC" /FO "$WORK/SculptMesh.res" "$ROOT/max/src/SculptMesh.rc"; then
    echo "ok    max/src/SculptMesh.rc"
  else
    echo "FAIL  max/src/SculptMesh.rc"
    status=1
  fi
else
  echo "skip  max/src/SculptMesh.rc (llvm-rc not found)"
fi

exit $status
