add_executable(mcp_app_fixture mcp_app_fixture.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/app_ipc_endpoint.cpp ${OKUFLOW_MCP_COMMON}
    ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_service.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/assistant_parameter_policy.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/operation_tracker.cpp)
target_include_directories(mcp_app_fixture PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(mcp_app_fixture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE OKUFLOW_MCP_TEST_HOOKS=1)
target_compile_definitions(mcp_app_fixture PRIVATE OKUFLOW_CONTROL_CATALOG_EDITION_INDEX=${OKUFLOW_CONTROL_CATALOG_EDITION_INDEX})
target_compile_options(mcp_app_fixture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(mcp_app_fixture PRIVATE Qt6::Core Qt6::Gui advapi32 bcrypt shell32 ole32)
if(OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY)
    set(MCP_FIXTURE_IMAGE_NAME OkuFlowApp)
else()
    set(MCP_FIXTURE_IMAGE_NAME oku_flow)
endif()
set_target_properties(mcp_app_fixture PROPERTIES OUTPUT_NAME ${MCP_FIXTURE_IMAGE_NAME} RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mcp-fixture")
add_dependencies(mcp_app_fixture okuflow_mcp)
add_custom_command(TARGET mcp_app_fixture POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:okuflow_mcp> $<TARGET_FILE_DIR:mcp_app_fixture>/okuflow_mcp.exe)
add_executable(mcp_tests mcp_tests.cpp ${OKUFLOW_MCP_COMMON}
    ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp)
target_include_directories(mcp_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(mcp_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    OKUFLOW_CONTROL_CATALOG_EDITION_INDEX=${OKUFLOW_CONTROL_CATALOG_EDITION_INDEX}
    OKUFLOW_MCP_APP_FIXTURE="$<TARGET_FILE:mcp_app_fixture>"
    OKUFLOW_MCP_BRIDGE_FIXTURE="$<TARGET_FILE_DIR:mcp_app_fixture>/okuflow_mcp.exe")
target_compile_options(mcp_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(mcp_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test advapi32 bcrypt shell32 ole32)
add_dependencies(mcp_tests mcp_app_fixture)
add_test(NAME mcp_bridge_contract COMMAND mcp_tests)
set_tests_properties(mcp_bridge_contract PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
include("${PROJECT_SOURCE_DIR}/cmake/PowerShell.cmake")
okuflow_find_powershell(OKUFLOW_PWSH_EXECUTABLE)
add_test(NAME mcp_bundle_contract COMMAND ${OKUFLOW_PWSH_EXECUTABLE} -NoProfile -File
    ${PROJECT_SOURCE_DIR}/scripts/test_mcp_bundle.ps1)
add_test(NAME bundle_layout_contract COMMAND ${OKUFLOW_PWSH_EXECUTABLE} -NoProfile -File
    ${PROJECT_SOURCE_DIR}/scripts/test_bundle_layout.ps1)
add_test(NAME mcp_native_import_contract COMMAND ${OKUFLOW_PWSH_EXECUTABLE} -NoProfile -File
    ${PROJECT_SOURCE_DIR}/scripts/validate_mcp_release_build.ps1 -NativeOnly -Executable $<TARGET_FILE:okuflow_mcp>)
set_tests_properties(mcp_bundle_contract bundle_layout_contract mcp_native_import_contract PROPERTIES TIMEOUT 30)
