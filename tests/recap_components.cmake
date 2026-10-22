# Selected-media components retain the production sources and native ownership.
# Offscreen widgets do not claim interactive desktop or screen-reader acceptance.
if(NOT WIN32)
    return()
endif()

include(original_recording.cmake)
include(video_playback.cmake)
include(video_recap.cmake)
include(uct_screen_text_accessibility.cmake)

# Every direct writer consumer needs its observed byte-stream implementation.
get_property(recap_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(recap_target IN LISTS recap_targets)
    get_target_property(recap_sources ${recap_target} SOURCES)
    if("${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp" IN_LIST recap_sources)
        if(NOT "${PROJECT_SOURCE_DIR}/src/common/video_memory_stream.cpp" IN_LIST recap_sources)
            target_sources(${recap_target} PRIVATE ${PROJECT_SOURCE_DIR}/src/common/video_memory_stream.cpp)
        endif()
        if(NOT "${PROJECT_SOURCE_DIR}/src/common/video_archive.cpp" IN_LIST recap_sources)
            target_sources(${recap_target} PRIVATE ${PROJECT_SOURCE_DIR}/src/common/video_archive.cpp)
        endif()
        target_link_libraries(${recap_target} PRIVATE propsys)
    endif()
    if("${PROJECT_SOURCE_DIR}/src/ui/main_window.cpp" IN_LIST recap_sources)
        if(NOT "${PROJECT_SOURCE_DIR}/src/ui/video_transport.cpp" IN_LIST recap_sources)
            target_sources(${recap_target} PRIVATE
                ${PROJECT_SOURCE_DIR}/src/ui/video_transport.cpp
                ${PROJECT_SOURCE_DIR}/include/okuflow/ui/video_transport.hpp)
        endif()
    endif()
endforeach()

get_target_property(immutable_uct_sources ultra_clear_text_tests SOURCES)
list(REMOVE_ITEM immutable_uct_sources ultra_clear_text_tests.cpp)
add_executable(ultra_clear_text_controller_tests ultra_clear_text_controller_tests.cpp ${immutable_uct_sources})
target_include_directories(ultra_clear_text_controller_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(ultra_clear_text_controller_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(ultra_clear_text_controller_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
add_test(NAME immutable_uct_contract COMMAND ultra_clear_text_controller_tests)
# Aggregate deadline for the whole Qt suite; individual QTRY limits remain.
# The loaded CI runner completed 31 cases before the old 60 s suite deadline.
set_tests_properties(immutable_uct_contract PROPERTIES TIMEOUT 120 LABELS "headless"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

add_test(NAME uct_timing_controller_contract
    COMMAND ultra_clear_text_controller_tests timingTraceKeepsWorkerAndAdmissionTickets)
set_tests_properties(uct_timing_controller_contract PROPERTIES TIMEOUT 30 LABELS "headless"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1;OKUFLOW_UCT_TIMING_TRACE=1"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    REQUIRED_FILES "${PROJECT_SOURCE_DIR}/include/okuflow/common/uct_timing_trace.hpp;${PROJECT_SOURCE_DIR}/src/common/uct_timing_trace.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/ultra_clear_text.hpp;${PROJECT_SOURCE_DIR}/src/common/ultra_clear_text.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/app/ultra_clear_text_controller.hpp;${PROJECT_SOURCE_DIR}/src/app/ultra_clear_text_controller.cpp;${PROJECT_SOURCE_DIR}/tests/ultra_clear_text_controller_tests.cpp")

add_test(NAME uct_timing_pane_publication_contract
    COMMAND ultra_clear_text_controller_tests timingPaneOnlyChangePreservesPositionedTicket)
set_tests_properties(uct_timing_pane_publication_contract PROPERTIES TIMEOUT 30 LABELS "headless"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1;OKUFLOW_UCT_TIMING_TRACE=1"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    REQUIRED_FILES "${PROJECT_SOURCE_DIR}/include/okuflow/common/uct_timing_trace.hpp;${PROJECT_SOURCE_DIR}/src/common/uct_timing_trace.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/app/ultra_clear_text_controller.hpp;${PROJECT_SOURCE_DIR}/src/app/ultra_clear_text_controller.cpp;${PROJECT_SOURCE_DIR}/tests/ultra_clear_text_controller_tests.cpp")

add_executable(video_transport_tests video_transport_tests.cpp
    ${PROJECT_SOURCE_DIR}/assets/okuflow_resources.qrc
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/video_transport.hpp
    ${PROJECT_SOURCE_DIR}/src/ui/video_transport.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp)
target_include_directories(video_transport_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(video_transport_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(video_transport_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
add_test(NAME video_transport_contract COMMAND video_transport_tests)
set_tests_properties(video_transport_contract PROPERTIES TIMEOUT 30 LABELS "headless"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

add_executable(video_audio_output_tests video_audio_output_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/common/video_audio_output.cpp)
target_include_directories(video_audio_output_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(video_audio_output_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(video_audio_output_tests PRIVATE ole32)
add_test(NAME video_audio_output_contract COMMAND video_audio_output_tests)
set_tests_properties(video_audio_output_contract PROPERTIES TIMEOUT 30 LABELS "headless"
    REQUIRED_FILES "${PROJECT_SOURCE_DIR}/include/okuflow/common/video_audio_output.hpp;${PROJECT_SOURCE_DIR}/src/common/video_audio_output.cpp")

add_executable(video_agent_control_tests video_agent_control_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/parameter_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_service.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/recording_parameter_policy.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/operation_tracker.cpp)
target_include_directories(video_agent_control_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(video_agent_control_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(video_agent_control_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME video_agent_control_contract COMMAND video_agent_control_tests)
set_tests_properties(video_agent_control_contract PROPERTIES TIMEOUT 30 LABELS "headless"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

foreach(recap_qt_test immutable_uct_contract video_transport_contract video_agent_control_contract recording_original_native)
    set_property(TEST ${recap_qt_test} APPEND PROPERTY ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
endforeach()
