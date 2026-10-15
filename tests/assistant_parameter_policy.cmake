add_executable(assistant_parameter_policy_tests assistant_parameter_policy_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/assistant_parameter_policy.cpp)
target_include_directories(assistant_parameter_policy_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(assistant_parameter_policy_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_compile_options(assistant_parameter_policy_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME assistant_parameter_policy_contract COMMAND assistant_parameter_policy_tests)
set_tests_properties(assistant_parameter_policy_contract PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
