# Tests for plugin code that does not depend on the 3ds Max SDK. Built on
# every platform, unlike the plugin itself.
add_executable(sculpt_max_logic_tests
  ${PROJECT_SOURCE_DIR}/core/tests/test_main.cpp
  ${PROJECT_SOURCE_DIR}/max/src/SculptSettings.cpp
  test_settings.cpp
)
target_include_directories(sculpt_max_logic_tests PRIVATE ${PROJECT_SOURCE_DIR}/max/src)
target_link_libraries(sculpt_max_logic_tests PRIVATE SculptCore)
sculpt_configure_target(sculpt_max_logic_tests)
add_test(NAME sculpt_max_logic_tests COMMAND sculpt_max_logic_tests)
