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
list(FILTER OKUFLOW_CPP_SOURCES EXCLUDE REGEX "/src/launcher/")
list(FILTER OKUFLOW_CPP_SOURCES EXCLUDE REGEX "/src/mcp/")
if (OKUFLOW_ENABLE_CUDA)
    file(GLOB_RECURSE OKUFLOW_CUDA_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cu)
endif()

add_executable(oku_flow ${OKUFLOW_CPP_SOURCES})

if (OKUFLOW_ENABLE_CUDA)
    target_sources(oku_flow PRIVATE ${OKUFLOW_CUDA_SOURCES})
    set_target_properties(oku_flow PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RUNTIME_LIBRARY Static
        CUDA_ARCHITECTURES "75;80;86;89;90;100;103;120;121"
    )
endif()

target_sources(oku_flow PRIVATE
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/app.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/app_language.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/assistive_feature_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/color_schemes.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/constants.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/interaction_controller.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/language_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/pipeline_orchestrator.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/protected_secret_store.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/recording_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/recording_session_info.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/settings_controller.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/settings_store.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/suspend_guard.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/ui_state_manager.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/user_data_paths.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/capture/audio_capture.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/capture/adb_owned_server.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/capture/adb_legacy_recovery.hpp
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
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_agent_client.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/http_agent_client.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/visible_chat_session.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/codex_json_rpc_process.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/d3d12/presenter.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/main_window.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/agent_control_panel.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistant_chat_view.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/assistive_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/annotation_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/joystick_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/live_status_text.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/render_widget.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/color_scheme_picker.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/collapsible_section.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/responsive_slider_row.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ui_translation.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/theme_contrast.hpp
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
    # Explorer/Start-menu launches are GUI-only; --console attaches an existing
    # terminal for opt-in diagnostics without creating a console window.
    WIN32_EXECUTABLE $<BOOL:${WIN32}>
)

target_compile_definitions(oku_flow PRIVATE
    OKUFLOW_ENABLE_CUDA=$<BOOL:${OKUFLOW_ENABLE_CUDA}>
    OKUFLOW_HAS_CUDA_EXT_MEMORY=$<IF:$<BOOL:${OKUFLOW_ENABLE_CUDA}>,1,0>
    OKUFLOW_ENABLE_TEXT_SR=$<BOOL:${OKUFLOW_ENABLE_TEXT_SR}>
)

if (WIN32)
    include(WindowsRuntime.cmake)
    # Ninja's Windows RC rule does not discover the ICON file dependency.
    # Recompile both executable resources whenever the shared icon changes.
    set_source_files_properties(${PROJECT_SOURCE_DIR}/assets/okuflow.rc PROPERTIES
        OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/assets/okuflow.ico")
    target_include_directories(oku_flow PRIVATE ${OKUFLOW_RUNTIME_GENERATED_DIR})
    add_executable(okuflow_launcher WIN32
        ${PROJECT_SOURCE_DIR}/src/launcher/main.cpp
        ${PROJECT_SOURCE_DIR}/src/app/windows_runtime_contract.cpp
        ${PROJECT_SOURCE_DIR}/assets/okuflow.rc)
    target_include_directories(okuflow_launcher PRIVATE ${PROJECT_SOURCE_DIR}/include ${OKUFLOW_RUNTIME_GENERATED_DIR})
    target_compile_definitions(okuflow_launcher PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    set_property(TARGET okuflow_launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(okuflow_launcher PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(okuflow_launcher PRIVATE advapi32 version bcrypt shell32 comctl32)
    target_sources(oku_flow PRIVATE
        ${PROJECT_SOURCE_DIR}/assets/okuflow.rc
        ${PROJECT_SOURCE_DIR}/assets/okuflow_resources.qrc
    )

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
        iphlpapi
        ws2_32
        d3d11
        d3d12
        dxgi
        mfplat
        mfreadwrite
        wmcodecdspuuid
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
        version
        bcrypt
    )

    target_compile_definitions(oku_flow PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

    # Keep in sync with src/app/app_language.cpp and scripts/check_translations.ps1.
    set(OKUFLOW_TS_FILES
        ${PROJECT_SOURCE_DIR}/translations/okuflow_tr.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_de.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_fr.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_es.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_it.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_pt_BR.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_ru.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_uk.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_ar.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_hi.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_ja.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_ko.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_zh_Hans.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_nl.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_pl.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_cs.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_sv.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_id.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_vi.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_th.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_bn.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_fa.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_he.ts
        ${PROJECT_SOURCE_DIR}/translations/okuflow_ur.ts
    )
    qt_add_lupdate(
        LUPDATE_TARGET update_okuflow_translations
        SOURCE_TARGETS oku_flow
        TS_FILES
            ${OKUFLOW_TS_FILES}
        OPTIONS -locations none
    )
    qt_add_lrelease(
        LRELEASE_TARGET release_okuflow_translations
        TS_FILES
            ${OKUFLOW_TS_FILES}
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

if (WIN32)
    include(Mcp.cmake)
endif()

if (OKUFLOW_ENABLE_TESTS)
    if (EXISTS "${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(${PROJECT_SOURCE_DIR}/tests ${CMAKE_BINARY_DIR}/tests)
    else()
        message(WARNING "OKUFLOW_ENABLE_TESTS=ON, but no tests/CMakeLists.txt exists yet.")
    endif()
endif()
