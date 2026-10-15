add_executable(fake_scoped_codex_vision fake_scoped_codex_vision.cpp)
target_link_libraries(fake_scoped_codex_vision PRIVATE Qt6::Core)
target_compile_definitions(fake_scoped_codex_vision PRIVATE
    OKUFLOW_AGENT_PROFILE_FIXTURE="${PROJECT_SOURCE_DIR}/assets/codex-agent/0.160.1-profile.json")
target_compile_options(fake_scoped_codex_vision PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_dependencies(assistive_preparation_tests fake_scoped_codex_vision)
