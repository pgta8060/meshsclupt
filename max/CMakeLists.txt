# SculptMesh.dlo — the 3ds Max plugin. Windows + MSVC only.
# Configure with -DMAXSDK_DIR=<path to the SDK's "maxsdk" folder>.

if(NOT MSVC)
  message(FATAL_ERROR "The 3ds Max plugin must be built with MSVC (Visual Studio 2022, v143).")
endif()
if(NOT EXISTS "${MAXSDK_DIR}/include/max.h")
  message(FATAL_ERROR "MAXSDK_DIR='${MAXSDK_DIR}' does not look like a 3ds Max SDK (include/max.h not found).")
endif()

set(MAXSDK_LIB_DIR "${MAXSDK_DIR}/lib/x64/Release")

add_library(SculptMeshMax SHARED
  src/ConvertToSculpt.cpp
  src/DllEntry.cpp
  src/SculptInterface.cpp
  src/SculptDisplay.cpp
  src/SculptMeshObject.cpp
  src/SculptMode.cpp
  src/SculptPanel.cpp
  src/SculptSessionBridge.cpp
  src/SculptSettings.cpp
  src/SculptMesh.rc
  src/SculptMesh.def
)

set_target_properties(SculptMeshMax PROPERTIES
  OUTPUT_NAME "SculptMesh"
  SUFFIX ".dlo"
  # 3ds Max loads plugins against the release CRT, also in debug builds.
  MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

target_include_directories(SculptMeshMax PRIVATE src "${MAXSDK_DIR}/include")
target_link_directories(SculptMeshMax PRIVATE "${MAXSDK_LIB_DIR}")

# Settings mirrored from the SDK's 3dsmax.cpp.props.
target_compile_definitions(SculptMeshMax PRIVATE
  UNICODE _UNICODE WIN32 WIN64 _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX _ADESK_3DSMAX_WINDOWS_
  WINVER=0x0601 _WIN32_WINNT=0x0601 _WIN32_WINDOWS=0x0601 _WIN32_IE=0x0800
  _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE _SCL_SECURE_NO_DEPRECATE
  ISOLATION_AWARE_ENABLED=1 MODULE_NAME=SculptMesh.dlo
)
target_compile_options(SculptMeshMax PRIVATE
  /GR /EHa /fp:precise /permissive- /Zc:__cplusplus /Zc:inline /Zc:lambda /utf-8 /W3 /MP
  /wd4018 /wd4244 /wd4456 /wd4457 /wd4458 /wd4459
)

target_link_libraries(SculptMeshMax PRIVATE
  SculptCore
  core geom gfx mesh mnmath maxutil maxscrpt paramblk2 poly bmm
  graphicsdriver databridge maxgraphicsobjects optimesh
  comctl32
)

# Package layout: <prefix>/plugins/SculptMesh.dlo, <prefix>/macroscripts/*.mcr
install(TARGETS SculptMeshMax RUNTIME DESTINATION plugins LIBRARY DESTINATION plugins)
install(FILES scripts/SculptMesh_Macros.mcr DESTINATION macroscripts)

# Optional: copy the plugin straight into a 3ds Max plugin folder after each build.
set(SCULPT_MAX_PLUGIN_DIR "" CACHE PATH "If set, SculptMesh.dlo is copied here after building")
if(SCULPT_MAX_PLUGIN_DIR)
  add_custom_command(TARGET SculptMeshMax POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SculptMeshMax> "${SCULPT_MAX_PLUGIN_DIR}"
    COMMENT "Copying SculptMesh.dlo to ${SCULPT_MAX_PLUGIN_DIR}")
endif()
