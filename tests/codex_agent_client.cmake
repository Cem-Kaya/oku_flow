add_executable(fake_codex_agent_server fake_codex_agent_server.cpp)
target_link_libraries(fake_codex_agent_server PRIVATE Qt6::Core)
target_compile_definitions(fake_codex_agent_server PRIVATE
    OKUFLOW_AGENT_PROFILE_FIXTURE="${PROJECT_SOURCE_DIR}/assets/codex-agent/0.160.1-profile.json")
add_executable(codex_agent_client_tests codex_agent_client_tests.cpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_agent_client.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
    ${PROJECT_SOURCE_DIR}/src/common/codex_agent_client.cpp
    ${PROJECT_SOURCE_DIR}/src/common/codex_agent_profile.cpp
    ${PROJECT_SOURCE_DIR}/src/common/codex_json_rpc_process.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/parameter_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/operation_tracker.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/camera_source_intent.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_service.cpp)
target_include_directories(codex_agent_client_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(codex_agent_client_tests PRIVATE
    OKUFLOW_FAKE_AGENT_BINARY="$<TARGET_FILE:fake_codex_agent_server>")
target_compile_options(codex_agent_client_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(codex_agent_client_tests PRIVATE Qt6::Core Qt6::Test)
add_dependencies(codex_agent_client_tests fake_codex_agent_server)
add_test(NAME codex_agent_client_contract COMMAND codex_agent_client_tests)
set_tests_properties(codex_agent_client_contract PROPERTIES TIMEOUT 60 LABELS "headless"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
