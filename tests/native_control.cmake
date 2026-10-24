option(OKUFLOW_ENABLE_NATIVE_CONTROL_FIXTURE "Build opt-in actual-app offscreen control acceptance (CPU dev-full/v1)" OFF)
if(OKUFLOW_ENABLE_NATIVE_CONTROL_FIXTURE)
    if(OKUFLOW_ENABLE_CUDA OR NOT OKUFLOW_CONTROL_CATALOG_EDITION MATCHES "^(dev-full|v1)$")
        message(FATAL_ERROR "Native control fixture requires explicit CPU dev-full or v1 configuration; it does not establish shipping acceptance")
    endif()
    get_target_property(_native_sources oku_flow SOURCES)
    list(FILTER _native_sources EXCLUDE REGEX "/src/app/main\\.cpp$")
    add_executable(native_app_control_fixture EXCLUDE_FROM_ALL native_app_control_fixture.cpp ${_native_sources})
    foreach(_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS)
        get_target_property(_value oku_flow ${_property})
        if(_value)
            set_property(TARGET native_app_control_fixture PROPERTY ${_property} "${_value}")
        endif()
    endforeach()
    target_link_libraries(native_app_control_fixture PRIVATE Qt6::Test)
    target_compile_definitions(native_app_control_fixture PRIVATE OKUFLOW_VIDEO_DOCUMENT_TESTS=1)
    target_compile_definitions(native_app_control_fixture PRIVATE
        OKUFLOW_FAKE_AGENT_BINARY="$<TARGET_FILE:fake_codex_agent_server>")
    add_dependencies(native_app_control_fixture fake_codex_agent_server)
    set_target_properties(native_app_control_fixture PROPERTIES WIN32_EXECUTABLE FALSE OUTPUT_NAME ${MCP_FIXTURE_IMAGE_NAME}
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/native-control")
    add_dependencies(native_app_control_fixture release_okuflow_translations okuflow_mcp)
    add_custom_command(TARGET native_app_control_fixture POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:okuflow_mcp> $<TARGET_FILE_DIR:native_app_control_fixture>/okuflow_mcp.exe)
    add_test(NAME native_app_control_offscreen COMMAND native_app_control_fixture)
    set_tests_properties(native_app_control_offscreen PROPERTIES TIMEOUT 60 LABELS "headless;native-control-opt-in"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
    add_test(NAME native_app_recap_followup_offscreen COMMAND native_app_control_fixture
        collapsedRecapViewportSpaceKeepsFreezeResumeAndFocus
        terminalRecapResumeRetainsHeldScene warmingRecapResumeStillWaitsForGenuineHistory)
    set_tests_properties(native_app_recap_followup_offscreen PROPERTIES TIMEOUT 30 LABELS "headless;native-control-opt-in"
        RESOURCE_LOCK okuflow_process_identity
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
        REQUIRED_FILES "${PROJECT_SOURCE_DIR}/tests/native_app_control_fixture.cpp;${PROJECT_SOURCE_DIR}/src/ui/main_window.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/ui/video_transport.hpp;${PROJECT_SOURCE_DIR}/src/ui/video_transport.cpp;${PROJECT_SOURCE_DIR}/src/app/app_video_playback.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/video_recap.hpp")
    add_test(NAME native_app_view_controls_offscreen COMMAND native_app_control_fixture
        zoomOutRangeAndEndpointUseActualAspectWithoutPersistence
        freezeKeepsCommittedFillBeforeDeferredFitRequest
        freezeOriginalFallbackPreservesCommittedIntent
        pauseRetainsPresentedOriginalBeforeDeferredSuccessor
        videoChevronLeadsNeutralAndActionsContainOnlyFourButtons)
    set_tests_properties(native_app_view_controls_offscreen PROPERTIES TIMEOUT 30
        LABELS "headless;native-control-opt-in" RESOURCE_LOCK okuflow_process_identity
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
    add_test(NAME native_app_assistant_routing_offscreen COMMAND native_app_control_fixture
        assistantSendPreparesToolsAndContinuesWithoutRoutingButtons
        assistantPreparationCanBeStoppedOrReconfigured
        assistantSendSeparatesRestoredProviderHistory
        assistantFailedPreparationNeverDispatchesControl
        assistantHttpPreparationContinuesOrFailsWithoutDispatch
        uctMasterAndOriginalUseNativeControllerWithoutClaimingReadiness)
    set_tests_properties(native_app_assistant_routing_offscreen PROPERTIES TIMEOUT 60
        LABELS "headless;native-control-opt-in" RESOURCE_LOCK okuflow_process_identity
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
        REQUIRED_FILES "$<TARGET_FILE:fake_codex_agent_server>;${PROJECT_SOURCE_DIR}/assets/codex-agent/0.160.1-profile.json;${PROJECT_SOURCE_DIR}/tests/native_app_control_fixture.cpp")
    set_property(TEST native_app_control_offscreen APPEND PROPERTY REQUIRED_FILES
        "$<TARGET_FILE:fake_codex_agent_server>" "${PROJECT_SOURCE_DIR}/assets/codex-agent/0.160.1-profile.json")
endif()
