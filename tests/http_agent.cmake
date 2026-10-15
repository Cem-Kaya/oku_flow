add_executable(http_agent_client_tests
    http_agent_client_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/common/http_agent_client.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/parameter_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_service.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/operation_tracker.cpp)
target_include_directories(http_agent_client_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(http_agent_client_tests PRIVATE Qt6::Core Qt6::Network Qt6::Test)
target_compile_options(http_agent_client_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME http_agent_client_contract COMMAND http_agent_client_tests)
set_tests_properties(http_agent_client_contract PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
