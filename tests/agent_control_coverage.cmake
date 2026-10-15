# Plan 52 expanded-control coverage review contracts. Desktop-free: the
# production Qt-free registry, both schema validators, the pure image and
# assistant planners and AppControlService with a labelled fake target. It
# opens no window, camera, provider, account, pipe or settings file.
add_executable(agent_control_coverage_tests
    ${CMAKE_CURRENT_LIST_DIR}/agent_control_coverage_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/parameter_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_service.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_image_parameters.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_ui_parameters.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/assistant_parameter_policy.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/recording_parameter_policy.cpp
    ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp
    ${PROJECT_SOURCE_DIR}/src/app/color_schemes.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/json.cpp)
target_include_directories(agent_control_coverage_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
# Compiles only the image planner's pure dependency seam, without OkuFlowApp.
target_compile_definitions(agent_control_coverage_tests PRIVATE
    OKUFLOW_IMAGE_PARAMETER_PLANNER_ONLY OKUFLOW_UI_PARAMETER_PLANNER_ONLY NOMINMAX)
target_link_libraries(agent_control_coverage_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_compile_options(agent_control_coverage_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME agent_control_coverage_contract COMMAND agent_control_coverage_tests)
set_tests_properties(agent_control_coverage_contract PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
