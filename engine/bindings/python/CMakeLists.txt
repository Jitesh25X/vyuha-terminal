# Python 3.13 per TECH_STACK.md; override with -DPython_EXECUTABLE=... (e.g. uv's interpreter from M1.1).
find_package(Python 3.13...<3.14 COMPONENTS Interpreter Development.Module REQUIRED)
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(vyuha_core NB_STATIC module.cc)
target_link_libraries(vyuha_core PRIVATE vyuha::core vyuha_warnings)

if(VYUHA_BUILD_TESTS)
  add_test(NAME python.import_vyuha_core
    COMMAND ${Python_EXECUTABLE} -c "import vyuha_core; assert vyuha_core.version() == '0.0.1', vyuha_core.version()")
  set_tests_properties(python.import_vyuha_core PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:vyuha_core>")
endif()
