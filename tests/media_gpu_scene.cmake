# Window-free GPU ownership fixture for MediaGpuScene. Patterned on
# maxine_navigation_tests in maxine_lifecycle.cmake: same production CUDA
# surface sources, private fake Maxine SDK headers for compilation only.
# Neural ownership uses the pinned private fake runtime; no installed SDK
# can be probed. Fake publication is not real Maxine quality/performance proof.
if(WIN32 AND OKUFLOW_ENABLE_CUDA)
    set(media_gpu_scene_fixture_include "${PROJECT_SOURCE_DIR}/third_party/maxine/Maxine-VFX-SDK/nvvfx/include")
    add_executable(media_gpu_scene_tests media_gpu_scene_tests.cpp
        "${PROJECT_SOURCE_DIR}/src/app/media_gpu_scene.cpp"
        "${PROJECT_SOURCE_DIR}/src/d3d12/presenter.cpp"
        "${PROJECT_SOURCE_DIR}/src/common/maxine_async_producer.cpp"
        "${PROJECT_SOURCE_DIR}/src/cuda/cuda_interop.cpp"
        "${PROJECT_SOURCE_DIR}/src/cuda/uct_analysis_pool.cpp"
        "${PROJECT_SOURCE_DIR}/src/cuda/uct_kernels.cu"
        "${PROJECT_SOURCE_DIR}/src/cuda/cuda_kernels.cu"
        "${PROJECT_SOURCE_DIR}/src/cuda/spatial_upscalers.cu"
        "${PROJECT_SOURCE_DIR}/src/common/maxine_superres.cpp"
        "${PROJECT_SOURCE_DIR}/src/common/ultra_clear_text.cpp"
        "${PROJECT_SOURCE_DIR}/src/common/uct_region_planner.cpp"
        "${PROJECT_SOURCE_DIR}/src/common/uct_timing_trace.cpp"
        "${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp")
    target_include_directories(media_gpu_scene_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${media_gpu_scene_fixture_include}")
    target_compile_definitions(media_gpu_scene_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN OKUFLOW_ENABLE_CUDA=1
        OKUFLOW_HAS_CUDA_EXT_MEMORY=1 OKUFLOW_ENABLE_TEXT_SR=1)
    if(TARGET maxine_fake_image AND TARGET maxine_fake_effects)
        target_compile_definitions(media_gpu_scene_tests PRIVATE
            OKUFLOW_MAXINE_FAKE_IMAGE="$<TARGET_FILE:maxine_fake_image>"
            OKUFLOW_MAXINE_FAKE_EFFECTS="$<TARGET_FILE:maxine_fake_effects>")
        add_dependencies(media_gpu_scene_tests maxine_fake_image maxine_fake_effects)
        set(media_gpu_scene_required_files
            "$<TARGET_FILE:maxine_fake_image>;$<TARGET_FILE:maxine_fake_effects>;")
    endif()
    target_compile_options(media_gpu_scene_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    target_link_libraries(media_gpu_scene_tests PRIVATE
        Qt6::Core Qt6::Gui CUDA::cudart_static CUDA::cuda_driver d3d11 d3d12 dxgi d3dcompiler windowscodecs advapi32)
    set_target_properties(media_gpu_scene_tests PROPERTIES
        CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static
        CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}")
    add_test(NAME media_gpu_scene_cuda COMMAND media_gpu_scene_tests)
    set_tests_properties(media_gpu_scene_cuda PROPERTIES
        TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS gpu RUN_SERIAL TRUE
        REQUIRED_FILES "${media_gpu_scene_required_files}${PROJECT_SOURCE_DIR}/src/app/media_gpu_scene.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/app/media_gpu_scene.hpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/media_frame_layout.hpp"
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
endif()
