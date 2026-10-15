add_executable(codex_agent_profile_tests codex_agent_profile_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/common/codex_agent_profile.cpp)
target_include_directories(codex_agent_profile_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(codex_agent_profile_tests PRIVATE
    OKUFLOW_AGENT_PROFILE_FIXTURE="${PROJECT_SOURCE_DIR}/assets/codex-agent/0.160.1-profile.json")
target_compile_options(codex_agent_profile_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(codex_agent_profile_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME codex_agent_profile_contract COMMAND codex_agent_profile_tests)
set_tests_properties(codex_agent_profile_contract PROPERTIES TIMEOUT 15 LABELS "headless"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
