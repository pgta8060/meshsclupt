add_library(SculptCore STATIC
  src/alpha.cpp
  src/brush.cpp
  src/bvh.cpp
  src/display_chunks.cpp
  src/mesh.cpp
  src/multires.cpp
  src/parallel.cpp
  src/poly.cpp
  src/session.cpp
  src/session_ops.cpp
  src/stroke.cpp
  src/subdivide.cpp
  src/symmetry.cpp
)
target_include_directories(SculptCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
target_link_libraries(SculptCore PUBLIC Threads::Threads)
set_target_properties(SculptCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
sculpt_configure_target(SculptCore)

if(SCULPT_BUILD_TESTS)
  add_executable(sculpt_tests
    tests/test_main.cpp
    tests/test_brush.cpp
    tests/test_bvh.cpp
    tests/test_display.cpp
    tests/test_features.cpp
    tests/test_math.cpp
    tests/test_mesh.cpp
    tests/test_multires.cpp
    tests/test_session.cpp
    tests/test_stroke.cpp
  )
  target_link_libraries(sculpt_tests PRIVATE SculptCore)
  sculpt_configure_target(sculpt_tests)
  add_test(NAME sculpt_tests COMMAND sculpt_tests)
endif()

if(SCULPT_BUILD_BENCH)
  add_executable(sculpt_bench bench/bench_main.cpp)
  target_include_directories(sculpt_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(sculpt_bench PRIVATE SculptCore)
  sculpt_configure_target(sculpt_bench)
endif()
