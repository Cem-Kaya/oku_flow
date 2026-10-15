add_executable(image_parameter_planner_tests
    image_parameter_planner_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_image_parameters.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/parameter_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp)
target_include_directories(image_parameter_planner_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(image_parameter_planner_tests PRIVATE OKUFLOW_IMAGE_PARAMETER_PLANNER_ONLY=1)
target_compile_options(image_parameter_planner_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(image_parameter_planner_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME image_parameter_planner_contract COMMAND image_parameter_planner_tests)
set_tests_properties(image_parameter_planner_contract PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
