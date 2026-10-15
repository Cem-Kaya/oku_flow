add_executable(agent_control_tests
    agent_control_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/parameter_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_service.cpp)
target_sources(agent_control_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/app/agent/operation_tracker.cpp)
target_sources(agent_control_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/app/agent/camera_source_intent.cpp)
target_sources(agent_control_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app/agent/observation_service.cpp
    ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)
target_include_directories(agent_control_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(agent_control_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_compile_options(agent_control_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME agent_control_contract COMMAND agent_control_tests)
get_target_property(AGENT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(AGENT_QT_BIN "${AGENT_QMAKE_EXECUTABLE}" DIRECTORY)
set_tests_properties(agent_control_contract PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AGENT_QT_BIN}")
