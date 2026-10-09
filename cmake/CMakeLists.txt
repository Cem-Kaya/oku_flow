set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(ProjectOptions.cmake)

# Enable CUDA if requested
if (OKUFLOW_ENABLE_CUDA)
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
else()
    message(STATUS "OKUFLOW_ENABLE_CUDA=OFF: building without CUDA support")
endif()

file(GLOB_RECURSE OKUFLOW_CPP_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cpp)
if (OKUFLOW_ENABLE_CUDA)
    file(GLOB_RECURSE OKUFLOW_CUDA_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cu)
endif()

add_executable(oku_flow ${OKUFLOW_CPP_SOURCES})

if (OKUFLOW_ENABLE_CUDA)
    target_sources(oku_flow PRIVATE ${OKUFLOW_CUDA_SOURCES})
    set_target_properties(oku_flow PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RUNTIME_LIBRARY Static
        CUDA_ARCHITECTURES "75;86;89"
    )
endif()

target_sources(oku_flow PRIVATE
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/app.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/assistive_feature_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/color_schemes.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/constants.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/interaction_controller.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/language_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/pipeline_orchestrator.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/protected_secret_store.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/recording_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/settings_controller.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/transcription_session_controller.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/settings_store.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/suspend_guard.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/ui_state_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/user_data_paths.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/capture/audio_capture.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/capture/media_capture.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/image_processing.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/frame_pipeline.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/media_writer.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/recording_contract.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/response_language.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/maxine_superres.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/annotation_model.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/view_transform.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/assistive_runtime.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_app_server_client.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_realtime_transcription_client.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/realtime_native_rtc_carrier.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/realtime_transcription_interfaces.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/transcript.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/d3d12/presenter.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/main_window.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistive_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/annotation_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/joystick_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/live_status_text.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/render_widget.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/color_scheme_picker.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/collapsible_section.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/responsive_slider_row.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ui_translation.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/wheel_safe_combo_box.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ai_settings_dialog.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/setup_assistant.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/cuda/cuda_interop.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/cuda/cuda_kernels.hpp
)

target_include_directories(oku_flow PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third_party/maxine/Maxine-VFX-SDK/nvvfx/include
)
set_target_properties(oku_flow PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    # Keep the console in Release while stabilization tuning is active. Runtime
    # diagnostics are rate-limited, and the console gives field testers enough
    # evidence to distinguish estimator, filter, and performance failures.
    WIN32_EXECUTABLE FALSE
)

target_compile_definitions(oku_flow PRIVATE
    OKUFLOW_ENABLE_CUDA=$<BOOL:${OKUFLOW_ENABLE_CUDA}>
    OKUFLOW_HAS_CUDA_EXT_MEMORY=$<IF:$<BOOL:${OKUFLOW_ENABLE_CUDA}>,1,0>
    OKUFLOW_ENABLE_TEXT_SR=$<BOOL:${OKUFLOW_ENABLE_TEXT_SR}>
)

if (WIN32)
    target_sources(oku_flow PRIVATE
        ${PROJECT_SOURCE_DIR}/assets/okuflow.rc
        ${PROJECT_SOURCE_DIR}/assets/okuflow_resources.qrc
    )

    # Live transcription is a shipping capability, not an optional build
    # variant. Keeping one Windows dependency graph avoids an untested moc/
    # linker split and ensures every release receives the same hardening.
    include(NativeRtc.cmake)
    target_link_libraries(oku_flow PRIVATE OkuFlow::NativeRtc)

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
        target_link_libraries(oku_flow PRIVATE Qt6::TextToSpeech)
        target_compile_definitions(oku_flow PRIVATE OKUFLOW_HAS_TTS=1)
    endif()

    if (OKUFLOW_ENABLE_CUDA)
        find_package(CUDAToolkit REQUIRED)
        target_link_libraries(oku_flow PRIVATE CUDA::cudart_static CUDA::cuda_driver)
    endif()

    target_link_libraries(oku_flow PRIVATE
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

    target_compile_definitions(oku_flow PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

    qt_add_lupdate(
        LUPDATE_TARGET update_okuflow_translations
        SOURCE_TARGETS oku_flow
        TS_FILES
            ${PROJECT_SOURCE_DIR}/translations/okuflow_tr.ts
            ${PROJECT_SOURCE_DIR}/translations/okuflow_de.ts
        OPTIONS -locations none
    )
    qt_add_lrelease(
        LRELEASE_TARGET release_okuflow_translations
        TS_FILES
            ${PROJECT_SOURCE_DIR}/translations/okuflow_tr.ts
            ${PROJECT_SOURCE_DIR}/translations/okuflow_de.ts
        QM_FILES_OUTPUT_VARIABLE OKUFLOW_QM_FILES
        OPTIONS -nounfinished
    )
    qt_add_resources(oku_flow "oku_flow_translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}"
        FILES ${OKUFLOW_QM_FILES}
    )

    if (OKUFLOW_ENABLE_CUDA)
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
    message(WARNING "OkuFlow is intended to be built on Windows with CUDA, Qt6, and Direct3D12 support.")
endif()

if (OKUFLOW_ENABLE_TESTS)
    if (EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(${PROJECT_SOURCE_DIR}/tests ${CMAKE_BINARY_DIR}/tests)
    else()
        message(WARNING "OKUFLOW_ENABLE_TESTS=ON, but no tests/CMakeLists.txt exists yet.")
    endif()
endif()
