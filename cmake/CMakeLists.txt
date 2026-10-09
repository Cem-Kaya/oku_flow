set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(ProjectOptions.cmake)

# Enable CUDA if requested
if (OPENZOOM_ENABLE_CUDA)
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
else()
    message(STATUS "OPENZOOM_ENABLE_CUDA=OFF: building without CUDA support")
endif()

file(GLOB_RECURSE OPENZOOM_CPP_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cpp)
if (OPENZOOM_ENABLE_CUDA)
    file(GLOB_RECURSE OPENZOOM_CUDA_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cu)
endif()

add_executable(open_zoom ${OPENZOOM_CPP_SOURCES})

if (OPENZOOM_ENABLE_CUDA)
    target_sources(open_zoom PRIVATE ${OPENZOOM_CUDA_SOURCES})
    set_target_properties(open_zoom PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RUNTIME_LIBRARY Static
        CUDA_ARCHITECTURES "75;86;89"
    )
endif()

target_sources(open_zoom PRIVATE
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/app.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/assistive_feature_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/color_schemes.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/constants.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/interaction_controller.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/language_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/pipeline_orchestrator.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/protected_secret_store.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/recording_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/settings_controller.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/transcription_session_controller.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/settings_store.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/suspend_guard.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/ui_state_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/user_data_paths.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/capture/audio_capture.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/capture/media_capture.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/image_processing.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/frame_pipeline.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/media_writer.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/recording_contract.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/response_language.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/maxine_superres.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/annotation_model.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/view_transform.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/assistive_runtime.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/codex_app_server_client.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/codex_json_rpc_process.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/codex_realtime_transcription_client.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/realtime_native_rtc_carrier.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/realtime_transcription_interfaces.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/common/transcript.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/d3d12/presenter.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/main_window.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/assistive_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/annotation_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/joystick_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/live_status_text.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/render_widget.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/color_scheme_picker.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/collapsible_section.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/responsive_slider_row.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/ui_translation.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/wheel_safe_combo_box.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/ai_settings_dialog.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/app/setup_assistant.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/cuda/cuda_interop.hpp
    ${PROJECT_SOURCE_DIR}/include/openzoom/cuda/cuda_kernels.hpp
)

target_include_directories(open_zoom PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party/maxine/Maxine-VFX-SDK/nvvfx/include
)
set_target_properties(open_zoom PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    # Keep the console in Release while stabilization tuning is active. Runtime
    # diagnostics are rate-limited, and the console gives field testers enough
    # evidence to distinguish estimator, filter, and performance failures.
    WIN32_EXECUTABLE FALSE
)

target_compile_definitions(open_zoom PRIVATE
    OPENZOOM_ENABLE_CUDA=$<BOOL:${OPENZOOM_ENABLE_CUDA}>
    OPENZOOM_HAS_CUDA_EXT_MEMORY=$<IF:$<BOOL:${OPENZOOM_ENABLE_CUDA}>,1,0>
    OPENZOOM_ENABLE_TEXT_SR=$<BOOL:${OPENZOOM_ENABLE_TEXT_SR}>
)

if (WIN32)
    target_sources(open_zoom PRIVATE
        ${PROJECT_SOURCE_DIR}/assets/openzoom.rc
        ${PROJECT_SOURCE_DIR}/assets/openzoom_resources.qrc
    )

    # Live transcription is a shipping capability, not an optional build
    # variant. Keeping one Windows dependency graph avoids an untested moc/
    # linker split and ensures every release receives the same hardening.
    include(NativeRtc.cmake)
    target_link_libraries(open_zoom PRIVATE OpenZoom::NativeRtc)

    find_package(Qt6 QUIET COMPONENTS Widgets Network LinguistTools)
    if (NOT Qt6_FOUND)
        message(FATAL_ERROR
            "Qt6 was not found. Set QT_PREFIX to the Qt msvc2022_64 root "
            "when using the Windows scripts, set Qt6_DIR to the Qt6 CMake "
            "package, or update msvc-base in cmake/CMakePresets.json.")
    endif()

    # Optional text-to-speech support (Qt TextToSpeech module).
    find_package(Qt6 QUIET COMPONENTS TextToSpeech)
    if (Qt6TextToSpeech_FOUND)
        target_link_libraries(open_zoom PRIVATE Qt6::TextToSpeech)
        target_compile_definitions(open_zoom PRIVATE OPENZOOM_HAS_TTS=1)
    endif()

    if (OPENZOOM_ENABLE_CUDA)
        find_package(CUDAToolkit REQUIRED)
        target_link_libraries(open_zoom PRIVATE CUDA::cudart_static CUDA::cuda_driver)
    endif()

    target_link_libraries(open_zoom PRIVATE
        Qt6::Widgets
        Qt6::Network
        d3d11
        d3d12
        dxgi
        mfplat
        mfreadwrite
        mfuuid
        dxguid
        d3dcompiler
        ole32
        mf
        shlwapi
        uuid
        Synchronization
        windowscodecs
        comctl32
        advapi32
    )

    target_compile_definitions(open_zoom PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

    qt_add_lupdate(
        LUPDATE_TARGET update_openzoom_translations
        SOURCE_TARGETS open_zoom
        TS_FILES
            ${PROJECT_SOURCE_DIR}/translations/openzoom_tr.ts
            ${PROJECT_SOURCE_DIR}/translations/openzoom_de.ts
        OPTIONS -locations none
    )
    qt_add_lrelease(
        LRELEASE_TARGET release_openzoom_translations
        TS_FILES
            ${PROJECT_SOURCE_DIR}/translations/openzoom_tr.ts
            ${PROJECT_SOURCE_DIR}/translations/openzoom_de.ts
        QM_FILES_OUTPUT_VARIABLE OPENZOOM_QM_FILES
        OPTIONS -nounfinished
    )
    qt_add_resources(open_zoom "open_zoom_translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}"
        FILES ${OPENZOOM_QM_FILES}
    )

    if (OPENZOOM_ENABLE_CUDA)
        set(SANDBOX_DIR ${PROJECT_SOURCE_DIR}/sandbox/dx12_cuda_minimal)
        if (EXISTS "${SANDBOX_DIR}/CMakeLists.txt")
            add_subdirectory(${SANDBOX_DIR} ${CMAKE_BINARY_DIR}/sandbox_dx12_cuda_minimal)
        endif()
    endif()

    set(MF_DXVA_SANDBOX_DIR ${PROJECT_SOURCE_DIR}/sandbox/mf_dxva_minimal)
    if (EXISTS "${MF_DXVA_SANDBOX_DIR}/CMakeLists.txt")
        add_subdirectory(${MF_DXVA_SANDBOX_DIR}
                         ${CMAKE_BINARY_DIR}/sandbox_mf_dxva_minimal)
    endif()
else()
    message(WARNING "OpenZoom is intended to be built on Windows with CUDA, Qt6, and Direct3D12 support.")
endif()

if (OPENZOOM_ENABLE_TESTS)
    if (EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(${PROJECT_SOURCE_DIR}/tests ${CMAKE_BINARY_DIR}/tests)
    else()
        message(WARNING "OPENZOOM_ENABLE_TESTS=ON, but no tests/CMakeLists.txt exists yet.")
    endif()
endif()
