add_executable(control_schema_tests control_schema_tests.cpp)
target_include_directories(control_schema_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(control_schema_tests PRIVATE Qt6::Core Qt6::Test)
target_compile_options(control_schema_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME control_schema_contract COMMAND control_schema_tests)
set_tests_properties(control_schema_contract PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
