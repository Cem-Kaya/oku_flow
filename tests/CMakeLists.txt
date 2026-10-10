find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)

add_executable(settings_store_tests
    settings_store_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/app/color_schemes.cpp
    ${PROJECT_SOURCE_DIR}/src/app/protected_secret_store.cpp
    ${PROJECT_SOURCE_DIR}/src/app/settings_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/app/settings_store.cpp
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
    add_executable(capture_shutdown_tests capture_shutdown_tests.cpp)
    target_include_directories(capture_shutdown_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(capture_shutdown_tests PRIVATE Qt6::Test)
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
    add_executable(language_manager_tests
        language_manager_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/app/language_manager.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/response_language.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/live_status_text.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ui_translation.hpp
        ${PROJECT_SOURCE_DIR}/src/app/language_manager.cpp
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

    add_executable(annotation_overlay_tests
        annotation_overlay_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/assistive_runtime.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_app_server_client.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
        ${PROJECT_SOURCE_DIR}/src/app/assistive_feature_manager.cpp
        ${PROJECT_SOURCE_DIR}/src/app/user_data_paths.cpp
        ${PROJECT_SOURCE_DIR}/src/common/assistive_runtime.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_app_server_client.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_json_rpc_process.cpp
        ${PROJECT_SOURCE_DIR}/src/common/response_language.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistive_overlay.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistive_overlay.cpp
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

    add_test(NAME annotation_overlay_interactions COMMAND annotation_overlay_tests)
    set_tests_properties(annotation_overlay_interactions PROPERTIES
        TIMEOUT 30
        LABELS "gui"
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )

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

    # Exercise the real MainWindow widget tree without constructing OkuFlowApp
    # or starting camera, account, and recording services. The test translation
    # resources come from the same lrelease output as the shipping application.
    add_executable(main_window_tests
        main_window_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/main_window.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/render_widget.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistive_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/annotation_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/joystick_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/color_scheme_picker.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/collapsible_section.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/ui/responsive_slider_row.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/app/language_manager.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/main_window.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/render_widget.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/assistive_overlay.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/annotation_overlay.cpp
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
        Qt6::Test d3d12 dxgi d3dcompiler)
    qt_add_resources(main_window_tests "okuflow_main_window_test_translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}/../cmake"
        FILES ${OKUFLOW_QM_FILES}
    )
    add_test(NAME main_window_interactions COMMAND main_window_tests)
    set_tests_properties(main_window_interactions PROPERTIES TIMEOUT 90 LABELS "gui"
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(notes_html_tests
        notes_html_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/assistive_runtime.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_app_server_client.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
        ${PROJECT_SOURCE_DIR}/src/common/assistive_runtime.cpp
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
    add_dependencies(notes_html_tests fake_codex_app_server)

    add_executable(codex_realtime_client_tests
        codex_realtime_client_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_app_server_client.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_realtime_transcription_client.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/realtime_transcription_interfaces.hpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_app_server_client.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_json_rpc_process.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_realtime_transcription_client.cpp
    )
    target_include_directories(codex_realtime_client_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(codex_realtime_client_tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_dependencies(codex_realtime_client_tests fake_codex_app_server)

    add_test(NAME codex_realtime_protocol COMMAND codex_realtime_client_tests)
    set_tests_properties(codex_realtime_protocol PROPERTIES
        TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )

    add_executable(transcription_controller_tests
        transcription_controller_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/app/transcription_session_controller.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/realtime_transcription_interfaces.hpp
        ${PROJECT_SOURCE_DIR}/src/app/transcription_session_controller.cpp
    )
    target_include_directories(transcription_controller_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(transcription_controller_tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    target_compile_definitions(transcription_controller_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    add_test(NAME transcription_controller COMMAND transcription_controller_tests)
    set_tests_properties(transcription_controller PROPERTIES
        TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )

    add_executable(native_rtc_carrier_tests
        native_rtc_carrier_tests.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/realtime_native_rtc_carrier.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/realtime_transcription_interfaces.hpp
        ${PROJECT_SOURCE_DIR}/src/common/realtime_native_rtc_carrier.cpp
    )
    target_include_directories(native_rtc_carrier_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(native_rtc_carrier_tests PRIVATE
        Qt6::Core
        Qt6::Test
        OkuFlow::NativeRtc
    )
    target_compile_definitions(native_rtc_carrier_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    add_test(NAME native_rtc_carrier COMMAND native_rtc_carrier_tests)
    set_tests_properties(native_rtc_carrier PROPERTIES
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )

    # Opt-in live gate (plan 36 D4): needs a signed-in Codex CLI and network,
    # so it is built but never registered with ctest. Run manually with a
    # 48 kHz mono PCM16 WAV.
    add_executable(native_rtc_live_probe
        native_rtc_live_probe.cpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/app/transcription_session_controller.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_app_server_client.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_realtime_transcription_client.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/realtime_native_rtc_carrier.hpp
        ${PROJECT_SOURCE_DIR}/include/okuflow/common/realtime_transcription_interfaces.hpp
        ${PROJECT_SOURCE_DIR}/src/app/transcription_session_controller.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_app_server_client.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_json_rpc_process.cpp
        ${PROJECT_SOURCE_DIR}/src/common/codex_realtime_transcription_client.cpp
        ${PROJECT_SOURCE_DIR}/src/common/realtime_native_rtc_carrier.cpp
    )
    target_include_directories(native_rtc_live_probe PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(native_rtc_live_probe PRIVATE
        Qt6::Core
        OkuFlow::NativeRtc
    )
    target_compile_definitions(native_rtc_live_probe PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

if (WIN32 AND OKUFLOW_ENABLE_CUDA)
    add_executable(yuv_color_cuda_tests
        yuv_color_cuda_tests.cu
        ${PROJECT_SOURCE_DIR}/src/cuda/cuda_kernels.cu
    )
    target_include_directories(yuv_color_cuda_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(yuv_color_cuda_tests PRIVATE CUDA::cudart_static)
    set_target_properties(yuv_color_cuda_tests PROPERTIES
        CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON CUDA_ARCHITECTURES "75;86;89")
    add_test(NAME yuv_color_conversion_cuda COMMAND yuv_color_cuda_tests)
    set_tests_properties(yuv_color_conversion_cuda PROPERTIES
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
        CUDA_ARCHITECTURES "75;86;89"
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
        CUDA_ARCHITECTURES "75;86;89"
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
        CUDA_ARCHITECTURES "75;86;89"
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

endif()
