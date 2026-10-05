cmake_minimum_required(VERSION 3.20)

project(SculptMesh VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SCULPT_BUILD_TESTS "Build the SculptCore unit tests" ON)
option(SCULPT_BUILD_BENCH "Build the SculptCore benchmark" ON)
option(SCULPT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (GCC/Clang)" ON)
set(SCULPT_SANITIZE "" CACHE STRING "Sanitizers for GCC/Clang builds, e.g. 'address,undefined'")
set(MAXSDK_DIR "" CACHE PATH "Path to the 3ds Max SDK 'maxsdk' folder (enables the plugin build on Windows)")

# 3ds Max plugins must use the release DLL CRT (/MD) in every configuration,
# and the core library is linked into the plugin, so it follows the same rule.
if(MSVC AND MAXSDK_DIR)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Shared warning/sanitizer settings for first-party targets.
function(sculpt_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHa /fp:precise)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
                                             -Wcast-align -Wnull-dereference -Wdouble-promotion)
    if(SCULPT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(SCULPT_SANITIZE)
      target_compile_options(${target} PRIVATE -fsanitize=${SCULPT_SANITIZE} -fno-omit-frame-pointer
                                               -fno-sanitize-recover=all)
      target_link_options(${target} PRIVATE -fsanitize=${SCULPT_SANITIZE})
    endif()
  endif()
endfunction()

if(SCULPT_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)
if(SCULPT_BUILD_TESTS)
  add_subdirectory(max/tests)
endif()

if(MAXSDK_DIR)
  if(WIN32)
    add_subdirectory(max)
  else()
    message(STATUS "MAXSDK_DIR is set but the 3ds Max plugin only builds on Windows; "
                   "use tools/maxsdk_check to syntax-check it on this platform.")
  endif()
endif()
