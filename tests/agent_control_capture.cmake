# Explicit screenshot fixture, never part of the default native GUI gate.
get_target_property(AGENT_CAPTURE_SOURCES main_window_tests SOURCES)
list(FILTER AGENT_CAPTURE_SOURCES EXCLUDE REGEX "(^|/)main_window_tests.cpp$")
add_executable(agent_control_capture EXCLUDE_FROM_ALL agent_control_capture.cpp ${AGENT_CAPTURE_SOURCES}
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ai_settings_dialog.hpp
    ${PROJECT_SOURCE_DIR}/src/ui/ai_settings_dialog.cpp)
target_include_directories(agent_control_capture PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(agent_control_capture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE OKUFLOW_HAS_TTS=0)
target_compile_options(agent_control_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(agent_control_capture PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test d3d12 dxgi d3dcompiler)
add_dependencies(agent_control_capture main_window_tests)
