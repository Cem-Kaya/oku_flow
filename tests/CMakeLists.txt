find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
include(gui_cases.cmake)
include(headless_cases.cmake)
add_test(NAME cuda_target_policy
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/cuda_target_policy.cmake")

add_executable(deadline_worker_tests deadline_worker_tests.cpp)
target_include_directories(deadline_worker_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(deadline_worker_tests PRIVATE Qt6::Test)
add_test(NAME deadline_worker COMMAND deadline_worker_tests)

add_executable(retained_cleanup_tests retained_cleanup_tests.cpp)
target_include_directories(retained_cleanup_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(retained_cleanup_tests PRIVATE OKUFLOW_ASYNC_CLEANUP_TEST_HOOKS=1)
set_target_properties(retained_cleanup_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF DISABLE_PRECOMPILE_HEADERS ON)
add_test(NAME retained_cleanup_contract COMMAND retained_cleanup_tests)
set_tests_properties(retained_cleanup_contract PROPERTIES TIMEOUT 20 LABELS "headless"
    REQUIRED_FILES "${PROJECT_SOURCE_DIR}/include/okuflow/common/async_cleanup.hpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/retained_cleanup.hpp;${PROJECT_SOURCE_DIR}/src/cuda/uct_device_analysis.hpp;${PROJECT_SOURCE_DIR}/tests/retained_cleanup_tests.cpp")

add_executable(settings_store_tests
    settings_store_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/app/color_schemes.cpp
    ${PROJECT_SOURCE_DIR}/src/app/protected_secret_store.cpp
    ${PROJECT_SOURCE_DIR}/src/app/settings_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/app/settings_store.cpp
    ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp
)

target_include_directories(settings_store_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(settings_store_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    advapi32
)

add_test(NAME settings_store_round_trip COMMAND settings_store_tests)
get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIRECTORY "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
include(codex_agent_profile.cmake)
include(codex_agent_client.cmake)
include(agent_control.cmake)
include(control_schema.cmake)
include(control_diagnostics.cmake)
include(assistant_parameter_policy.cmake)
include(agent_control_coverage.cmake)
include(image_parameter_planner.cmake)
include(agent_control_panel.cmake)
include(http_agent.cmake)
include(visible_chat.cmake)
include(visible_chat_store.cmake)
include(mcp.cmake)
include(bundle_publication.cmake)
include(native_control.cmake)
include(ultra_clear_text.cmake)
include(uct_frozen_upload.cmake)
include(maxine_lifecycle.cmake)
include(media_gpu_scene.cmake)
set_tests_properties(deadline_worker PROPERTIES TIMEOUT 15
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
add_executable(launcher_fixture launcher_fixture.cpp)
set_property(TARGET launcher_fixture PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_executable(windows_runtime_tests windows_runtime_tests.cpp ${PROJECT_SOURCE_DIR}/src/app/windows_runtime_contract.cpp)
target_include_directories(windows_runtime_tests PRIVATE ${PROJECT_SOURCE_DIR}/include ${OKUFLOW_RUNTIME_GENERATED_DIR})
target_compile_definitions(windows_runtime_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    OKUFLOW_LAUNCHER_TEST_BINARY="$<TARGET_FILE:okuflow_launcher>"
    OKUFLOW_LAUNCHER_TEST_CHILD="$<TARGET_FILE:launcher_fixture>")
target_link_libraries(windows_runtime_tests PRIVATE Qt6::Core Qt6::Test advapi32 version bcrypt shell32)
add_dependencies(windows_runtime_tests okuflow_launcher launcher_fixture)
target_compile_options(windows_runtime_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME windows_runtime_contract COMMAND windows_runtime_tests)
set_tests_properties(windows_runtime_contract PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
add_executable(setup_assistant_tests setup_assistant_tests.cpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/setup_assistant.hpp
    ${PROJECT_SOURCE_DIR}/src/app/setup_assistant.cpp
    ${PROJECT_SOURCE_DIR}/src/app/windows_runtime.cpp
    ${PROJECT_SOURCE_DIR}/src/app/windows_runtime_contract.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp)
target_include_directories(setup_assistant_tests PRIVATE ${PROJECT_SOURCE_DIR}/include ${OKUFLOW_RUNTIME_GENERATED_DIR})
target_compile_definitions(setup_assistant_tests PRIVATE OKUFLOW_ENABLE_CUDA=0 NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(setup_assistant_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(setup_assistant_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test
    advapi32 version bcrypt shell32)
qt_add_resources(setup_assistant_tests "okuflow_setup_test_translations"
    PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}/../cmake" FILES ${OKUFLOW_QM_FILES})
add_test(NAME setup_assistant_interactions COMMAND setup_assistant_tests)
set_tests_properties(setup_assistant_interactions PROPERTIES TIMEOUT 30 LABELS "gui"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
add_executable(phone_retry_policy_tests phone_retry_policy_tests.cpp)
target_include_directories(phone_retry_policy_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(phone_retry_policy_tests PRIVATE Qt6::Core Qt6::Test)
add_test(NAME phone_retry_policy COMMAND phone_retry_policy_tests)
set_tests_properties(phone_retry_policy PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
set_tests_properties(settings_store_round_trip PROPERTIES
    TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
)

add_executable(user_data_paths_tests
    user_data_paths_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/app/user_data_paths.cpp
)

target_include_directories(user_data_paths_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(user_data_paths_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME user_data_paths COMMAND user_data_paths_tests)
set_tests_properties(user_data_paths PROPERTIES
    TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
)

if (WIN32)
    add_executable(adb_legacy_fixture adb_legacy_fixture.cpp adb_legacy_fixture.rc)
    target_compile_definitions(adb_legacy_fixture PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(adb_legacy_fixture PRIVATE ws2_32)
    set_property(TARGET adb_legacy_fixture PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_executable(adb_legacy_recovery_tests adb_legacy_recovery_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/adb_legacy_recovery.cpp)
    target_include_directories(adb_legacy_recovery_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(adb_legacy_recovery_tests PRIVATE
        Qt6::Core Qt6::Test iphlpapi ws2_32 version advapi32)
    target_compile_definitions(adb_legacy_recovery_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        OKUFLOW_ADB_LEGACY_FIXTURE="$<TARGET_FILE:adb_legacy_fixture>")
    add_dependencies(adb_legacy_recovery_tests adb_legacy_fixture)
    add_test(NAME adb_legacy_recovery COMMAND adb_legacy_recovery_tests -v1)
    set_tests_properties(adb_legacy_recovery PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
    add_executable(adb_owned_child_fixture
        adb_owned_child_fixture.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/adb_owned_server.cpp)
    target_include_directories(adb_owned_child_fixture PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(adb_owned_child_fixture PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(adb_owned_child_fixture PRIVATE iphlpapi ws2_32)
    set_property(TARGET adb_owned_child_fixture PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_executable(phone_camera_tests
        phone_camera_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/adb_owned_server.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/adb_legacy_recovery.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/phone_camera.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/media_capture_facade.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/media_capture.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/capture_buffer.cpp)
    target_include_directories(phone_camera_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(phone_camera_tests PRIVATE
        Qt6::Core Qt6::Network Qt6::Test iphlpapi ws2_32 version advapi32 d3d11 d3d12 dxgi mf mfplat mfreadwrite mfuuid ole32 wmcodecdspuuid)
    target_compile_definitions(phone_camera_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        OKUFLOW_ADB_OWNED_CHILD_FIXTURE="$<TARGET_FILE:adb_owned_child_fixture>")
    add_dependencies(phone_camera_tests adb_owned_child_fixture)
    add_test(NAME phone_camera_contract COMMAND phone_camera_tests -v1)
    set_tests_properties(phone_camera_contract PROPERTIES TIMEOUT 180
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(capture_shutdown_tests
        capture_shutdown_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/adb_owned_server.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/adb_legacy_recovery.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/media_capture_facade.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/media_capture.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/phone_camera.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/capture_buffer.cpp
    )
    target_include_directories(capture_shutdown_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(capture_shutdown_tests PRIVATE
        Qt6::Core Qt6::Network Qt6::Test iphlpapi ws2_32 version advapi32 d3d11 d3d12 dxgi mf mfplat mfreadwrite mfuuid ole32 wmcodecdspuuid)
    target_compile_definitions(capture_shutdown_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME capture_shutdown COMMAND capture_shutdown_tests)
    set_tests_properties(capture_shutdown PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(gpu_copy_lease_tests gpu_copy_lease_tests.cpp)
    target_include_directories(gpu_copy_lease_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(gpu_copy_lease_tests PRIVATE Qt6::Test)
    add_test(NAME gpu_copy_lease COMMAND gpu_copy_lease_tests)
    set_tests_properties(gpu_copy_lease PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(fence_wait_tests fence_wait_tests.cpp)
    target_include_directories(fence_wait_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(fence_wait_tests PRIVATE Qt6::Test)
    add_test(NAME fence_wait COMMAND fence_wait_tests)
    set_tests_properties(fence_wait PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(spatial_cache_geometry_tests
        spatial_cache_geometry_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)
    target_include_directories(spatial_cache_geometry_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    add_test(NAME spatial_cache_geometry COMMAND spatial_cache_geometry_tests)
    set_tests_properties(spatial_cache_geometry PROPERTIES TIMEOUT 30)

    add_executable(yuv_color_tests
        yuv_color_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/common/image_processing.cpp
        ${PROJECT_SOURCE_DIR}/src/common/frame_pipeline.cpp
    )
    target_include_directories(yuv_color_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(yuv_color_tests PRIVATE Qt6::Core Qt6::Test mfplat mfuuid)
    target_compile_definitions(yuv_color_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME yuv_color_conversion COMMAND yuv_color_tests)
    set_tests_properties(yuv_color_conversion PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(cuda_surface_retry_tests cuda_surface_retry_tests.cpp)
    target_include_directories(cuda_surface_retry_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    add_test(NAME cuda_surface_retry COMMAND cuda_surface_retry_tests)
    set_tests_properties(cuda_surface_retry PROPERTIES TIMEOUT 30)

    add_executable(capture_buffer_tests
        capture_buffer_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/capture/capture_buffer.cpp
    )
    target_include_directories(capture_buffer_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(capture_buffer_tests PRIVATE
        Qt6::Core Qt6::Test mfplat mfuuid ole32 d3d11
    )
    target_compile_definitions(capture_buffer_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )
    add_test(NAME capture_buffer_layout COMMAND capture_buffer_tests)
    set_tests_properties(capture_buffer_layout PROPERTIES
        TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )

    add_executable(stabilization_viewing_zoom_tests
        stabilization_viewing_zoom_tests.cpp
    )
    target_include_directories(stabilization_viewing_zoom_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_compile_definitions(stabilization_viewing_zoom_tests PRIVATE
        OKUFLOW_HAS_CUDA_EXT_MEMORY=0
        NOMINMAX WIN32_LEAN_AND_MEAN
    )
    add_test(NAME stabilization_viewing_zoom
             COMMAND stabilization_viewing_zoom_tests)
    set_tests_properties(stabilization_viewing_zoom PROPERTIES TIMEOUT 30)

    add_executable(pipeline_orchestrator_tests
        pipeline_orchestrator_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/app/pipeline_orchestrator.cpp
    )
    target_include_directories(pipeline_orchestrator_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(pipeline_orchestrator_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Test
    )
    add_test(NAME pipeline_timing_percentiles
             COMMAND pipeline_orchestrator_tests)
    set_tests_properties(pipeline_timing_percentiles PROPERTIES
        TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )
endif()

add_executable(view_transform_tests
    view_transform_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
)

target_include_directories(view_transform_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(view_transform_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)

add_test(NAME viewport_transform_geometry COMMAND view_transform_tests)
set_tests_properties(viewport_transform_geometry PROPERTIES
    TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
)

add_executable(annotation_model_tests
    annotation_model_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/common/annotation_model.cpp
    ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
)

target_include_directories(annotation_model_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(annotation_model_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)

add_test(NAME annotation_model_geometry COMMAND annotation_model_tests)
set_tests_properties(annotation_model_geometry PROPERTIES
    TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
)

add_executable(live_status_text_tests
    live_status_text_tests.cpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/live_status_text.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ui_translation.hpp
    ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
)

target_include_directories(live_status_text_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(live_status_text_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
)

add_test(NAME live_accessible_status COMMAND live_status_text_tests)
set_tests_properties(live_accessible_status PROPERTIES
    TIMEOUT 30
    LABELS "gui"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
)

if (WIN32)
    add_executable(language_catalog_contract_tests
        language_catalog_contract_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/app/language_manager.hpp
        ${PROJECT_SOURCE_DIR}/src/app/language_manager.cpp
        ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp)
    target_include_directories(language_catalog_contract_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(language_catalog_contract_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    target_compile_definitions(language_catalog_contract_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    qt_add_resources(language_catalog_contract_tests "okuflow_catalog_contract_translations"
        PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}/../cmake" FILES ${OKUFLOW_QM_FILES})
    add_test(NAME language_catalog_contract COMMAND language_catalog_contract_tests)
    set_tests_properties(language_catalog_contract PROPERTIES TIMEOUT 30 LABELS "headless"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(language_manager_tests
        language_manager_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/app/language_manager.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/response_language.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/live_status_text.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ui_translation.hpp
        ${PROJECT_SOURCE_DIR}/src/app/language_manager.cpp
        ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp
        ${PROJECT_SOURCE_DIR}/src/common/response_language.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
    )
    target_include_directories(language_manager_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(language_manager_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Test
    )
    qt_add_resources(language_manager_tests "okuflow_test_translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}/../cmake"
        FILES ${OKUFLOW_QM_FILES}
    )
    add_test(NAME multilingual_live_switch COMMAND language_manager_tests)
    set_tests_properties(multilingual_live_switch PROPERTIES
        TIMEOUT 30
        LABELS "gui"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )

    add_executable(recording_integrity_tests
        recording_integrity_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp
        ${PROJECT_SOURCE_DIR}/src/common/frame_pipeline.cpp
        ${PROJECT_SOURCE_DIR}/src/common/image_processing.cpp
    )

    target_include_directories(recording_integrity_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(recording_integrity_tests PRIVATE
        Qt6::Core
        Qt6::Test
        mfplat
        mfuuid
    )
    target_compile_definitions(recording_integrity_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )

    add_test(NAME recording_integrity COMMAND recording_integrity_tests)
    set_tests_properties(recording_integrity PROPERTIES
        TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )

    add_executable(recording_manager_tests
        recording_manager_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/live_status_text.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ui_translation.hpp
        ${PROJECT_SOURCE_DIR}/src/app/recording_manager.cpp
        ${PROJECT_SOURCE_DIR}/src/app/user_data_paths.cpp
        ${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp
        ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp
        ${PROJECT_SOURCE_DIR}/src/common/annotation_model.cpp
        ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
    )
    target_include_directories(recording_manager_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(recording_manager_tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test
        d3d11 dxgi mf mfplat mfreadwrite mfuuid ole32)
    target_compile_definitions(recording_manager_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME recording_manager COMMAND recording_manager_tests)
    set_tests_properties(recording_manager PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(media_writer_audio_tests
        media_writer_audio_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp
        ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp
    )
    target_include_directories(media_writer_audio_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(media_writer_audio_tests PRIVATE
        d3d11
        dxgi
        mf
        mfplat
        mfreadwrite
        mfuuid
        ole32
    )
    target_compile_definitions(media_writer_audio_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    # Mandatory H.264 leg plus an AV1 leg that runs where a hardware AV1
    # encoder exists (production tries AV1 first) and skips cleanly
    # elsewhere. The timeout doubles as the wedge detector: a codec whose
    # first WriteSample blocks fails the gate instead of hanging it.
    add_test(NAME media_writer_codecs_aac
             COMMAND media_writer_audio_tests)
    set_tests_properties(media_writer_codecs_aac PROPERTIES
        TIMEOUT 60
        LABELS "codec"
    )

    add_executable(media_writer_gpu_tests
        media_writer_gpu_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp
        ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp
    )
    target_include_directories(media_writer_gpu_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(media_writer_gpu_tests PRIVATE
        d3d11
        d3d12
        dxgi
        mf
        mfplat
        mfreadwrite
        mfuuid
        ole32
    )
    target_compile_definitions(media_writer_gpu_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )

    # The GPU-fed submission path (StartGpu/AddGpuFrame -> shared D3D12
    # texture -> D3D11 video processor -> DXGI samples -> hardware encoder)
    # — where the 2026-07-30 first-WriteSample wedge lived. Skips cleanly
    # (77) where D3D12 sharing or the GPU-fed writer is unavailable; the
    # timeout is the wedge detector.
    add_test(NAME media_writer_gpu_codecs
             COMMAND media_writer_gpu_tests)
    set_tests_properties(media_writer_gpu_codecs PROPERTIES
        TIMEOUT 60
        SKIP_RETURN_CODE 77
        LABELS "codec"
    )

    # Deterministic event routing only; the offscreen platform never takes
    # desktop focus. Native annotation layouts remain a separate GUI gate.
    add_executable(annotation_key_routing_tests
        annotation_key_routing_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/annotation_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/live_status_text.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ui_translation.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/annotation_overlay.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/common/annotation_model.cpp
        ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
        ${PROJECT_SOURCE_DIR}/assets/okuflow_resources.qrc
    )
    target_include_directories(annotation_key_routing_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(annotation_key_routing_tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    target_compile_definitions(annotation_key_routing_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME annotation_key_routing_headless COMMAND annotation_key_routing_tests)
    set_tests_properties(annotation_key_routing_headless PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(annotation_overlay_tests
        annotation_overlay_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/assistive_runtime.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_app_server_client.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
        ${PROJECT_SOURCE_DIR}/src/app/assistive_feature_manager.cpp
        ${PROJECT_SOURCE_DIR}/src/app/user_data_paths.cpp
        ${PROJECT_SOURCE_DIR}/src/common/assistive_runtime.cpp
        ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_app_server_client.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_json_rpc_process.cpp
        ${PROJECT_SOURCE_DIR}/src/common/response_language.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistive_overlay.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistive_overlay.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistant_chat_view.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/agent_control_panel.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistant_chat_view.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/agent_control_panel.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/annotation_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/live_status_text.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ui_translation.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/annotation_overlay.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/common/annotation_model.cpp
        ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
        ${PROJECT_SOURCE_DIR}/assets/okuflow_resources.qrc
    )

    target_include_directories(annotation_overlay_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(annotation_overlay_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Test
    )
    target_compile_definitions(annotation_overlay_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

    okuflow_add_gui_cases(annotation_overlay_interactions annotation_overlay_tests AnnotationOverlayTests)

    add_executable(render_widget_tests
        render_widget_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/render_widget.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/render_widget.cpp
        ${PROJECT_SOURCE_DIR}/src/d3d12/presenter.cpp
        ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
    )
    target_include_directories(render_widget_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(render_widget_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
        OKUFLOW_HAS_CUDA_EXT_MEMORY=0)
    target_link_libraries(render_widget_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test
        d3d12 dxgi d3dcompiler)
    add_test(NAME viewport_resize_presentation COMMAND render_widget_tests)
    set_tests_properties(viewport_resize_presentation PROPERTIES TIMEOUT 30 LABELS "gui;gpu"
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(color_scheme_picker_tests
        color_scheme_picker_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/color_scheme_picker.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/app/language_manager.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/color_scheme_picker.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/app/color_schemes.cpp
        ${PROJECT_SOURCE_DIR}/src/app/language_manager.cpp
        ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp
    )
    target_include_directories(color_scheme_picker_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(color_scheme_picker_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(color_scheme_picker_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    qt_add_resources(color_scheme_picker_tests "okuflow_color_picker_test_translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}/../cmake"
        FILES ${OKUFLOW_QM_FILES}
    )
    add_test(NAME color_scheme_picker COMMAND color_scheme_picker_tests)
    set_tests_properties(color_scheme_picker PROPERTIES TIMEOUT 30 LABELS "gui"
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    # The production settings dialog needs only static Codex reference text;
    # the fixture supplies that seam and disables speech engine discovery.
    add_executable(ai_settings_dialog_tests
        ai_settings_dialog_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ai_settings_dialog.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/live_status_text.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ui_translation.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/ai_settings_dialog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
    )
    target_include_directories(ai_settings_dialog_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(ai_settings_dialog_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN OKUFLOW_HAS_TTS=0)
    target_link_libraries(ai_settings_dialog_tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    okuflow_add_gui_cases(ai_settings_dialog_interactions ai_settings_dialog_tests AiSettingsDialogTests)

    # Exercise the real MainWindow widget tree without constructing OkuFlowApp
    # or starting camera, account, and recording services. The test translation
    # resources come from the same lrelease output as the shipping application.
    add_executable(main_window_tests
        main_window_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/visible_chat_session.hpp
        ${PROJECT_SOURCE_DIR}/src/common/visible_chat_session.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/main_window.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/agent_control_panel.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistant_chat_view.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/render_widget.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistive_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/annotation_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/joystick_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/color_scheme_picker.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/collapsible_section.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/responsive_slider_row.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/app/language_manager.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/main_window.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/agent_control_panel.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistant_chat_view.cpp
        ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/render_widget.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistive_overlay.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/annotation_overlay.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ultra_clear_text_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ultra_clear_text_panel.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/ultra_clear_text_overlay.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ultra_clear_text_panel.cpp
        ${PROJECT_SOURCE_DIR}/src/common/uct_timing_trace.cpp
        ${PROJECT_SOURCE_DIR}/src/common/ultra_clear_text.cpp
        ${PROJECT_SOURCE_DIR}/src/common/uct_region_planner.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/joystick_overlay.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/color_scheme_picker.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/collapsible_section.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/responsive_slider_row.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/app/language_manager.cpp
        ${PROJECT_SOURCE_DIR}/src/app/color_schemes.cpp
        ${PROJECT_SOURCE_DIR}/src/common/annotation_model.cpp
        ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
        ${PROJECT_SOURCE_DIR}/src/d3d12/presenter.cpp
        ${PROJECT_SOURCE_DIR}/assets/okuflow_resources.qrc
    )
    target_include_directories(main_window_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(main_window_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
        UNICODE _UNICODE OKUFLOW_HAS_CUDA_EXT_MEMORY=0 OKUFLOW_ENABLE_TEXT_SR=0)
    target_link_libraries(main_window_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets
        Qt6::Test d3d12 dxgi d3dcompiler dwmapi)
    qt_add_resources(main_window_tests "okuflow_main_window_test_translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}/../cmake"
        FILES ${OKUFLOW_QM_FILES}
    )
    okuflow_add_gui_cases(main_window_interactions main_window_tests MainWindowTests)

    add_executable(notes_html_tests
        notes_html_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/assistive_runtime.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_app_server_client.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
        ${PROJECT_SOURCE_DIR}/src/common/assistive_runtime.cpp
        ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_app_server_client.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_json_rpc_process.cpp
        ${PROJECT_SOURCE_DIR}/src/common/response_language.cpp
    )
    target_include_directories(notes_html_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(notes_html_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Test
    )
    target_compile_definitions(notes_html_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    add_test(NAME notes_html COMMAND notes_html_tests)
    set_tests_properties(notes_html PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )

    # Deterministic stand-in for the Codex app-server; speaks JSONL on stdio.
    add_executable(fake_codex_app_server
        fake_codex_app_server.cpp
    )
    target_link_libraries(fake_codex_app_server PRIVATE Qt6::Core)
    target_compile_definitions(fake_codex_app_server PRIVATE
        OKUFLOW_AGENT_PROFILE_FIXTURE="${PROJECT_SOURCE_DIR}/assets/codex-agent/0.160.1-profile.json")
    add_dependencies(notes_html_tests fake_codex_app_server)

    add_executable(assistive_preparation_tests
        assistive_preparation_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/assistive_runtime.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_app_server_client.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
        ${PROJECT_SOURCE_DIR}/src/common/assistive_runtime.cpp
        ${PROJECT_SOURCE_DIR}/src/app/app_language.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_app_server_client.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_json_rpc_process.cpp
        ${PROJECT_SOURCE_DIR}/src/common/response_language.cpp
    )
    target_include_directories(assistive_preparation_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(assistive_preparation_tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Test)
    target_compile_definitions(assistive_preparation_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE OKUFLOW_HAS_TTS=0)
    add_dependencies(assistive_preparation_tests fake_codex_app_server)
    okuflow_add_headless_cases(assistive_preparation assistive_preparation_tests AssistivePreparationTests)

endif()

if (WIN32 AND OKUFLOW_ENABLE_CUDA)
    add_executable(yuv_color_cuda_tests
        yuv_color_cuda_tests.cu
        ${PROJECT_SOURCE_DIR}/src/cuda/cuda_kernels.cu
    )
    target_include_directories(yuv_color_cuda_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(yuv_color_cuda_tests PRIVATE CUDA::cudart_static)
    set_target_properties(yuv_color_cuda_tests PROPERTIES
        CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}")
    add_test(NAME yuv_color_conversion_cuda COMMAND yuv_color_cuda_tests)
    set_tests_properties(yuv_color_conversion_cuda PROPERTIES
        TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "gpu")

    add_executable(text_clarity_cuda_tests
        text_clarity_cuda_tests.cu
        ${PROJECT_SOURCE_DIR}/src/cuda/cuda_kernels.cu
    )
    target_include_directories(text_clarity_cuda_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(text_clarity_cuda_tests PRIVATE CUDA::cudart_static)
    set_target_properties(text_clarity_cuda_tests PROPERTIES
        CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}")
    add_test(NAME text_clarity_sauvola_cuda COMMAND text_clarity_cuda_tests)
    set_tests_properties(text_clarity_sauvola_cuda PROPERTIES
        TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "gpu")

    add_executable(stabilization_cuda_tests
        stabilization_cuda_tests.cu
        ${PROJECT_SOURCE_DIR}/src/cuda/cuda_kernels.cu
    )
    target_include_directories(stabilization_cuda_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(stabilization_cuda_tests PRIVATE CUDA::cudart_static)
    set_target_properties(stabilization_cuda_tests PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}"
    )
    add_test(NAME stabilization_similarity_cuda
             COMMAND stabilization_cuda_tests)
    set_tests_properties(stabilization_similarity_cuda PROPERTIES
        TIMEOUT 30
        SKIP_RETURN_CODE 77
        LABELS "gpu"
    )

    add_executable(spatial_upscaler_cuda_tests
        spatial_upscaler_cuda_tests.cu
        ${PROJECT_SOURCE_DIR}/src/cuda/spatial_upscalers.cu
    )
    target_include_directories(spatial_upscaler_cuda_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(spatial_upscaler_cuda_tests PRIVATE CUDA::cudart_static)
    set_target_properties(spatial_upscaler_cuda_tests PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}"
    )
    add_test(NAME spatial_upscaler_reference_cuda
             COMMAND spatial_upscaler_cuda_tests)
    set_tests_properties(spatial_upscaler_reference_cuda PROPERTIES
        TIMEOUT 30
        SKIP_RETURN_CODE 77
        LABELS "gpu"
    )

    add_executable(stabilization_replay_cuda
        stabilization_replay_cuda.cu
        ${PROJECT_SOURCE_DIR}/src/cuda/cuda_kernels.cu
    )
    target_include_directories(stabilization_replay_cuda PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(stabilization_replay_cuda PRIVATE CUDA::cudart_static)
    set_target_properties(stabilization_replay_cuda PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}"
    )

    add_test(NAME stabilization_replay_regression
        COMMAND stabilization_replay_cuda
            --synthetic-clamp-bump
            --output "${CMAKE_CURRENT_BINARY_DIR}/clamp_bump_replay_output.bgra"
            --trace "${CMAKE_CURRENT_BINARY_DIR}/clamp_bump_replay_trace.csv"
            --width 160
            --height 90)
    set_tests_properties(stabilization_replay_regression PROPERTIES
        TIMEOUT 120
        SKIP_RETURN_CODE 77
        LABELS "gpu"
    )

    # Require native SASS independently of driver/PTX compatibility.
    # The inventory is selected once by cmake/CudaTargets.cmake.
    find_program(OKUFLOW_CUOBJDUMP NAMES cuobjdump
        HINTS ${CUDAToolkit_BIN_DIR} "$ENV{CUDA_PATH}/bin")
    if(OKUFLOW_CUOBJDUMP)
        string(JOIN "," expected_cuda_architectures ${OKUFLOW_CUDA_ARCHITECTURES})
        add_test(NAME cuda_architecture_coverage
            COMMAND ${CMAKE_COMMAND}
                "-DOKUFLOW_CUOBJDUMP=${OKUFLOW_CUOBJDUMP}"
                "-DOKUFLOW_EXECUTABLE=$<TARGET_FILE:oku_flow>"
                "-DOKUFLOW_EXPECTED_ARCHS=${expected_cuda_architectures}"
                -P "${PROJECT_SOURCE_DIR}/tests/check_cuda_architectures.cmake")
        set_tests_properties(cuda_architecture_coverage PROPERTIES TIMEOUT 60)
    endif()
endif()

include(agent_control_capture.cmake)
# Also serialize the small GUI suites that still run in one process. RUN_SERIAL
# prevents overlap with non-GUI tests inside one CTest; the fixture lease also
# prevents two separate CTest processes from opening competing GUI windows.
foreach(runtime_fixture IN ITEMS annotation_overlay_tests notes_html_tests assistive_preparation_tests)
    if(TARGET ${runtime_fixture})
        target_sources(${runtime_fixture} PRIVATE
            ${PROJECT_SOURCE_DIR}/include/okuflow/common/visible_chat_session.hpp
            ${PROJECT_SOURCE_DIR}/src/common/visible_chat_session.cpp
            ${PROJECT_SOURCE_DIR}/src/common/codex_agent_profile.cpp)
    endif()
endforeach()
if(TARGET assistive_preparation_tests)
    include(scoped_codex_vision.cmake)
endif()
if(TARGET annotation_overlay_tests)
    target_sources(annotation_overlay_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistant_chat_view.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistant_chat_view.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/agent_control_panel.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/agent_control_panel.cpp)
endif()
get_property(okuflow_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS okuflow_tests)
    # Windows Qt otherwise routes some plain-text results to the debugger,
    # hiding failed assertions and skipped data rows from CTest's evidence.
    set_property(TEST "${test}" APPEND PROPERTY ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
    get_property(labels TEST "${test}" PROPERTY LABELS)
    if("gui" IN_LIST labels)
        set_tests_properties("${test}" PROPERTIES RUN_SERIAL TRUE RESOURCE_LOCK okuflow_desktop)
    endif()
endforeach()
if(WIN32)
    # The fail-closed legacy sibling scan observes short-lived OkuFlow-named
    # fixture images, including MCP and launcher tests outside its temp root.
    # Do not race image lookup with their exit; production safety stays intact.
    foreach(test IN ITEMS adb_legacy_recovery windows_runtime_contract
            mcp_bridge_contract native_app_control_offscreen)
        if(TEST "${test}")
            set_property(TEST "${test}" APPEND PROPERTY RESOURCE_LOCK okuflow_process_identity)
        endif()
    endforeach()
    include(video_gpu_snapshot.cmake)
    include(recap_components.cmake)
    include(shared_sources.cmake)
    include(precompiled_headers.cmake)
    include(runtime_inputs.cmake)
endif()
