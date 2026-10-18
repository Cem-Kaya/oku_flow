add_executable(agent_control_panel_tests
    agent_control_panel_tests.cpp
    ${PROJECT_SOURCE_DIR}/assets/okuflow_resources.qrc
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/color_scheme_picker.hpp
    ${PROJECT_SOURCE_DIR}/src/ui/color_scheme_picker.cpp
    ${PROJECT_SOURCE_DIR}/src/app/color_schemes.cpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/visible_chat_session.hpp
    ${PROJECT_SOURCE_DIR}/src/common/visible_chat_session.cpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/agent_control_panel.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistant_chat_view.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistive_overlay.hpp
    ${PROJECT_SOURCE_DIR}/src/ui/agent_control_panel.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/assistant_chat_view.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/assistive_overlay.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp)
target_include_directories(agent_control_panel_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(agent_control_panel_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_options(agent_control_panel_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME agent_control_panel_headless COMMAND agent_control_panel_tests)
set_tests_properties(agent_control_panel_headless PROPERTIES TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

# Explicit development diagnostic: no timing threshold, default build or CTest.
add_executable(assistant_chat_benchmark EXCLUDE_FROM_ALL
    assistant_chat_benchmark.cpp
    ${PROJECT_SOURCE_DIR}/assets/okuflow_resources.qrc
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistant_chat_view.hpp
    ${PROJECT_SOURCE_DIR}/src/ui/assistant_chat_view.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp)
target_include_directories(assistant_chat_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(assistant_chat_benchmark PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_options(assistant_chat_benchmark PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
