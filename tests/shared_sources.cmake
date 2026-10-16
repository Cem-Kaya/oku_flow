# Exact-contract CUDA reuse; preprocessing-based C++ groups are below.
# Keep compile contracts intact: even an otherwise unrelated fixture define is
# a separate variant. Resources and QObject headers stay on the executables, so
# RCC initialization and each fixture's AUTOMOC inventory are unchanged.
set(okuflow_test_compile_properties
    INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES
    COMPILE_FLAGS COMPILE_WARNING_AS_ERROR
    MSVC_RUNTIME_LIBRARY CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS
    POSITION_INDEPENDENT_CODE QT_SKIP_DEFAULT_TESTCASE_DIRS QT_TESTCASE_BUILDDIR
    CUDA_STANDARD CUDA_STANDARD_REQUIRED CUDA_ARCHITECTURES
    CUDA_SEPARABLE_COMPILATION CUDA_RUNTIME_LIBRARY)

function(okuflow_test_compile_contract target output)
    get_target_property(libraries ${target} LINK_LIBRARIES)
    set(usage_libraries "")
    foreach(library IN LISTS libraries)
        if(TARGET ${library})
            list(APPEND usage_libraries "${library}")
        endif()
    endforeach()
    list(SORT usage_libraries)
    set(contract "${usage_libraries}")
    foreach(property IN LISTS okuflow_test_compile_properties)
        get_target_property(value ${target} ${property})
        if(value MATCHES "-NOTFOUND$")
            set(value "")
        endif()
        if(property STREQUAL "COMPILE_DEFINITIONS")
            # Qt's Windows platform interface already supplies these two exact
            # definitions. Explicit repeats do not create a different variant.
            if(WIN32 AND "Qt6::Core" IN_LIST usage_libraries)
                list(REMOVE_ITEM value UNICODE _UNICODE)
            endif()
            list(SORT value)
        endif()
        string(APPEND contract "\n${property}=${value}")
    endforeach()
    set(${output} "${contract}" PARENT_SCOPE)
endfunction()

function(okuflow_share_test_sources name)
    cmake_parse_arguments(PARSE_ARGV 1 shared "" "" "TARGETS;SOURCES")
    list(GET shared_TARGETS 0 reference)
    okuflow_test_compile_contract(${reference} reference_contract)
    add_library(${name} OBJECT ${shared_SOURCES})
    set_target_properties(${name} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    foreach(property IN LISTS okuflow_test_compile_properties)
        get_target_property(value ${reference} ${property})
        if(NOT value STREQUAL "" AND NOT value MATCHES "-NOTFOUND$")
            set_property(TARGET ${name} PROPERTY ${property} "${value}")
        endif()
    endforeach()
    # Native import libraries have no compile usage requirements. Retain every
    # Qt dependency, including Qt Test's default testcase-directory definitions.
    get_target_property(libraries ${reference} LINK_LIBRARIES)
    foreach(library IN LISTS libraries)
        if(TARGET ${library})
            target_link_libraries(${name} PRIVATE ${library})
        endif()
    endforeach()
    foreach(target IN LISTS shared_TARGETS)
        okuflow_test_compile_contract(${target} contract)
        if(NOT contract STREQUAL reference_contract)
            message(FATAL_ERROR "${name}: ${target} differs from ${reference}; create a separate compile variant")
        endif()
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS shared_SOURCES)
            if(NOT source IN_LIST sources)
                message(FATAL_ERROR "${target} does not compile shared source ${source}")
            endif()
            list(REMOVE_ITEM sources "${source}")
        endforeach()
        set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:${name}>)
    endforeach()
endfunction()


include(${PROJECT_SOURCE_DIR}/cmake/SharedSourceEquivalence.cmake)

function(okuflow_share_equivalent_sources name)
    cmake_parse_arguments(PARSE_ARGV 1 shared "" "" "TARGETS;SOURCES")
    # The CPU application was measured with CUDA disabled. Its CUDA variant is
    # deliberately retained on the executable; test environments are unchanged.
    if(OKUFLOW_ENABLE_CUDA)
        list(REMOVE_ITEM shared_TARGETS oku_flow)
    endif()
    list(FILTER shared_TARGETS INCLUDE REGEX ".+")
    set(consumers "")
    foreach(target IN LISTS shared_TARGETS)
        if(TARGET ${target})
            list(APPEND consumers ${target})
        endif()
    endforeach()
    list(LENGTH consumers count)
    if(count LESS 2)
        return()
    endif()
    list(GET consumers 0 reference)
    add_library(${name} OBJECT ${shared_SOURCES})
    set_target_properties(${name} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
        DISABLE_PRECOMPILE_HEADERS ON)
    foreach(property IN LISTS okuflow_test_compile_properties)
        get_target_property(value ${reference} ${property})
        if(NOT value STREQUAL "" AND NOT value MATCHES "-NOTFOUND$")
            set_property(TARGET ${name} PROPERTY ${property} "${value}")
        endif()
    endforeach()
    get_target_property(libraries ${reference} LINK_LIBRARIES)
    foreach(library IN LISTS libraries)
        if(TARGET ${library})
            target_link_libraries(${name} PRIVATE ${library})
        endif()
    endforeach()
    foreach(target IN LISTS consumers)
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS shared_SOURCES)
            if(NOT source IN_LIST sources)
                message(FATAL_ERROR "${name}: ${target} no longer compiles ${source}")
            endif()
            # Source overrides cannot be reconstructed from a target witness.
            okuflow_check_shared_source_overrides("${source}" ${target})
            list(REMOVE_ITEM sources "${source}")
            # AUTOMOC used to discover some matching headers through the .cpp.
            # Preserve that input explicitly after removing the .cpp.
            string(REPLACE "/src/" "/include/okuflow/" header "${source}")
            string(REGEX REPLACE "\\.cpp$" ".hpp" header "${header}")
            if(EXISTS "${header}")
                file(READ "${header}" header_text)
                if(header_text MATCHES "Q_OBJECT|Q_GADGET|Q_NAMESPACE")
                    list(APPEND sources "${header}")
                endif()
            endif()
            file(APPEND "${OKUFLOW_EQUIVALENCE_MANIFEST}"
                "${name}\t${source}\t${target}\n")
        endforeach()
        list(REMOVE_DUPLICATES sources)
        set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:${name}>)
    endforeach()
    foreach(source IN LISTS shared_SOURCES)
        file(APPEND "${OKUFLOW_EQUIVALENCE_MANIFEST}"
            "${name}\t${source}\t${name}\n")
    endforeach()
endfunction()

# Groups measured with MSVC /EP; the CTest guard rechecks every member.
okuflow_share_equivalent_sources(okuflow_shared_app_control_image_parameters_1
    TARGETS agent_control_coverage_tests image_parameter_planner_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_image_parameters.cpp)

okuflow_share_equivalent_sources(okuflow_shared_app_control_service_1
    TARGETS codex_agent_client_tests agent_control_tests agent_control_coverage_tests http_agent_client_tests mcp_app_fixture
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/agent/app_control_service.cpp)

okuflow_share_equivalent_sources(okuflow_shared_assistant_parameter_policy_1
    TARGETS assistant_parameter_policy_tests agent_control_coverage_tests mcp_app_fixture
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/agent/assistant_parameter_policy.cpp)

okuflow_share_equivalent_sources(okuflow_shared_camera_source_intent_1
    TARGETS codex_agent_client_tests agent_control_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/agent/camera_source_intent.cpp)

okuflow_share_equivalent_sources(okuflow_shared_control_registry_1
    TARGETS codex_agent_client_tests agent_control_tests agent_control_coverage_tests image_parameter_planner_tests http_agent_client_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp)

okuflow_share_equivalent_sources(okuflow_shared_control_registry_2
    TARGETS mcp_app_fixture mcp_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/protocol.cpp
        ${PROJECT_SOURCE_DIR}/src/mcp/windows_pipe.cpp)

okuflow_share_equivalent_sources(okuflow_shared_operation_tracker_1
    TARGETS codex_agent_client_tests agent_control_tests http_agent_client_tests mcp_app_fixture
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/agent/operation_tracker.cpp)

okuflow_share_equivalent_sources(okuflow_shared_parameter_registry_1
    TARGETS codex_agent_client_tests agent_control_tests agent_control_coverage_tests image_parameter_planner_tests http_agent_client_tests mcp_app_fixture mcp_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/agent/parameter_registry.cpp)

okuflow_share_equivalent_sources(okuflow_shared_app_language_2
    TARGETS oku_flow settings_store_tests language_catalog_contract_tests language_manager_tests annotation_overlay_tests color_scheme_picker_tests main_window_tests notes_html_tests assistive_preparation_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp)

okuflow_share_equivalent_sources(okuflow_shared_app_language_1
    TARGETS agent_control_coverage_tests agent_control_capture
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp)

okuflow_share_equivalent_sources(okuflow_shared_color_schemes_1
    TARGETS oku_flow settings_store_tests color_scheme_picker_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/color_schemes.cpp)

okuflow_share_equivalent_sources(okuflow_shared_color_schemes_2
    TARGETS agent_control_coverage_tests agent_control_panel_tests agent_control_capture
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/color_schemes.cpp)

okuflow_share_equivalent_sources(okuflow_shared_language_manager_1
    TARGETS oku_flow language_catalog_contract_tests language_manager_tests color_scheme_picker_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/language_manager.cpp)

okuflow_share_equivalent_sources(okuflow_shared_pipeline_orchestrator_1
    TARGETS oku_flow pipeline_orchestrator_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/pipeline_orchestrator.cpp)

okuflow_share_equivalent_sources(okuflow_shared_recording_manager_1
    TARGETS oku_flow recording_manager_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/recording_manager.cpp
        ${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp)

okuflow_share_equivalent_sources(okuflow_shared_settings_controller_1
    TARGETS oku_flow settings_store_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/settings_controller.cpp)

okuflow_share_equivalent_sources(okuflow_shared_user_data_paths_1
    TARGETS oku_flow user_data_paths_tests recording_manager_tests annotation_overlay_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/user_data_paths.cpp)

okuflow_share_equivalent_sources(okuflow_shared_windows_runtime_contract_1
    TARGETS windows_runtime_tests setup_assistant_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/app/windows_runtime_contract.cpp)

okuflow_share_equivalent_sources(okuflow_shared_adb_legacy_recovery_1
    TARGETS oku_flow adb_legacy_recovery_tests phone_camera_tests capture_shutdown_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/capture/adb_legacy_recovery.cpp)

okuflow_share_equivalent_sources(okuflow_shared_adb_owned_server_1
    TARGETS oku_flow phone_camera_tests capture_shutdown_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/capture/adb_owned_server.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/media_capture.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/media_capture_facade.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/phone_camera.cpp)

okuflow_share_equivalent_sources(okuflow_shared_capture_buffer_1
    TARGETS oku_flow phone_camera_tests capture_shutdown_tests capture_buffer_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/capture/capture_buffer.cpp)

okuflow_share_equivalent_sources(okuflow_shared_annotation_model_1
    TARGETS oku_flow annotation_model_tests recording_manager_tests annotation_key_routing_tests annotation_overlay_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/annotation_model.cpp)

okuflow_share_equivalent_sources(okuflow_shared_assistive_runtime_1
    TARGETS annotation_overlay_tests notes_html_tests assistive_preparation_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/assistive_runtime.cpp)

okuflow_share_equivalent_sources(okuflow_shared_codex_agent_profile_1
    TARGETS oku_flow annotation_overlay_tests notes_html_tests assistive_preparation_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/codex_agent_profile.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_app_server_client.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_json_rpc_process.cpp)

okuflow_share_equivalent_sources(okuflow_shared_codex_agent_profile_2
    TARGETS codex_agent_profile_tests codex_agent_client_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/codex_agent_profile.cpp)

okuflow_share_equivalent_sources(okuflow_shared_frame_pipeline_1
    TARGETS oku_flow yuv_color_tests recording_integrity_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/frame_pipeline.cpp
        ${PROJECT_SOURCE_DIR}/src/common/image_processing.cpp)

okuflow_share_equivalent_sources(okuflow_shared_media_writer_1
    TARGETS media_writer_audio_tests media_writer_gpu_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp
        ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp)

okuflow_share_equivalent_sources(okuflow_shared_recording_contract_1
    TARGETS oku_flow recording_integrity_tests recording_manager_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp)

okuflow_share_equivalent_sources(okuflow_shared_response_language_1
    TARGETS oku_flow language_manager_tests annotation_overlay_tests notes_html_tests assistive_preparation_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/response_language.cpp)

okuflow_share_equivalent_sources(okuflow_shared_view_transform_1
    TARGETS oku_flow view_transform_tests annotation_model_tests recording_manager_tests annotation_key_routing_tests annotation_overlay_tests render_widget_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)

okuflow_share_equivalent_sources(okuflow_shared_view_transform_2
    TARGETS agent_control_tests agent_control_capture
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)

okuflow_share_equivalent_sources(okuflow_shared_visible_chat_session_1
    TARGETS oku_flow annotation_overlay_tests main_window_tests notes_html_tests assistive_preparation_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/visible_chat_session.cpp)

okuflow_share_equivalent_sources(okuflow_shared_visible_chat_session_2
    TARGETS agent_control_panel_tests visible_chat_session_tests visible_chat_session_store_tests agent_control_capture
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/visible_chat_session.cpp)

okuflow_share_equivalent_sources(okuflow_shared_presenter_1
    TARGETS oku_flow render_widget_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/d3d12/presenter.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/render_widget.cpp)

okuflow_share_equivalent_sources(okuflow_shared_agent_control_panel_1
    TARGETS oku_flow annotation_overlay_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/agent_control_panel.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistant_chat_view.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistive_overlay.cpp)

okuflow_share_equivalent_sources(okuflow_shared_agent_control_panel_2
    TARGETS agent_control_panel_tests agent_control_capture
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/agent_control_panel.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistant_chat_view.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/color_scheme_picker.cpp)

okuflow_share_equivalent_sources(okuflow_shared_annotation_overlay_1
    TARGETS oku_flow annotation_key_routing_tests annotation_overlay_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/annotation_overlay.cpp)

okuflow_share_equivalent_sources(okuflow_shared_collapsible_section_1
    TARGETS oku_flow main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/collapsible_section.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/joystick_overlay.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/responsive_slider_row.cpp)

okuflow_share_equivalent_sources(okuflow_shared_color_scheme_picker_1
    TARGETS oku_flow color_scheme_picker_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/color_scheme_picker.cpp)

okuflow_share_equivalent_sources(okuflow_shared_live_status_text_1
    TARGETS oku_flow live_status_text_tests language_manager_tests recording_manager_tests annotation_key_routing_tests annotation_overlay_tests ai_settings_dialog_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp)

okuflow_share_equivalent_sources(okuflow_shared_live_status_text_2
    TARGETS agent_control_panel_tests setup_assistant_tests agent_control_capture
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp)

okuflow_share_equivalent_sources(okuflow_shared_translation_catalog_1
    TARGETS oku_flow live_status_text_tests language_catalog_contract_tests language_manager_tests recording_manager_tests annotation_key_routing_tests annotation_overlay_tests color_scheme_picker_tests ai_settings_dialog_tests main_window_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp)

okuflow_share_equivalent_sources(okuflow_shared_json_1
    TARGETS agent_control_coverage_tests mcp_app_fixture mcp_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/mcp/json.cpp)

if(OKUFLOW_ENABLE_CUDA)
    # These four fixtures have identical nvcc flags and no per-fixture defines.
    # OBJECT inclusion preserves the original non-separable CUDA link behavior.
    okuflow_share_test_sources(okuflow_test_cuda_kernels
        TARGETS yuv_color_cuda_tests text_clarity_cuda_tests stabilization_cuda_tests
            stabilization_replay_cuda
        SOURCES ${PROJECT_SOURCE_DIR}/src/cuda/cuda_kernels.cu)
    set_target_properties(okuflow_test_cuda_kernels PROPERTIES
        CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}")
endif()
