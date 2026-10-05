// Force-included by tools/maxsdk_check only. Lets clang + mingw-w64 headers
// parse the 3ds Max SDK, which assumes MSVC. Never used for real builds.
#pragma once

#include "gnu_macros.h"  // Generated: GCC builtin macros clang drops in MS mode.

#include <cmath>
#include <excpt.h>
#include <windows.h>

// MSVC's <cmath> also declares these in namespace std; libstdc++ does not.
namespace std {
using ::fabsf;
using ::fabsl;
}  // namespace std

// MSVC-only structured-exception translator API used by MAXScript headers.
typedef void(__cdecl* _se_translator_function)(unsigned int, struct _EXCEPTION_POINTERS*);
_se_translator_function __cdecl _set_se_translator(_se_translator_function);
