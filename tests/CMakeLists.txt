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
    ${PROJECT_SOURCE_DIR}/include/openzoom/ui/live_status_text.hpp
    ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
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
        ${PROJECT_SOURCE_DIR}/include/openzoom/ui/annotation_overlay.hpp
        ${PROJECT_SOURCE_DIR}/include/openzoom/ui/live_status_text.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/annotation_overlay.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
        ${PROJECT_SOURCE_DIR}/src/common/annotation_model.cpp
        ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
        ${PROJECT_SOURCE_DIR}/assets/openzoom_resources.qrc
    )

    target_include_directories(annotation_overlay_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(annotation_overlay_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Test
    )

    add_test(NAME annotation_overlay_interactions COMMAND annotation_overlay_tests)
    set_tests_properties(annotation_overlay_interactions PROPERTIES
        TIMEOUT 30
        LABELS "gui"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    )
endif()

if (WIN32 AND OPENZOOM_ENABLE_CUDA)
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
