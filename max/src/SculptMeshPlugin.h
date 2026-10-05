// Sculpt Mesh for 3ds Max — shared plugin-wide declarations.
#pragma once

#include <max.h>
#include <iparamb2.h>
#include <polyobj.h>

#include "resource.h"

// Unique IDs. Never change them: they are stored in .max files and scripts.
#define SCULPTMESH_CLASS_ID Class_ID(0x5c7a2e91, 0x3b14d6f8)
#define SCULPTMESH_FP_INTERFACE_ID Interface_ID(0x2f6d1a40, 0x7e93c5b2)
#define SCULPTMESH_CID_SCULPT_MODE (CID_USER + 0x5C01)

// Plugin version shown to users and scripts.
#define SCULPTMESH_VERSION_STRING _T("0.1.0 (phase 1)")

extern HINSTANCE hInstance;

// Localised string from the plugin's string table (never null).
const TCHAR* GetString(int id);

ClassDesc2* GetSculptMeshObjectDesc();
