if(WIN32 AND OKUFLOW_ENABLE_CUDA)
    set(maxine_fixture_include "${PROJECT_SOURCE_DIR}/third_party/maxine/Maxine-VFX-SDK/nvvfx/include")
    foreach(module IN ITEMS image effects)
        add_library(maxine_fake_${module} SHARED maxine_fake_sdk.cpp)
        target_include_directories(maxine_fake_${module} PRIVATE "${maxine_fixture_include}")
        target_compile_definitions(maxine_fake_${module} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(maxine_fake_${module} PRIVATE CUDA::cuda_driver)
        set_target_properties(maxine_fake_${module} PROPERTIES
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/maxine-fixture"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/maxine-fixture")
    endforeach()
    target_compile_definitions(maxine_fake_image PRIVATE OKUFLOW_FAKE_IMAGE_MODULE=1)
    target_sources(maxine_fake_image PRIVATE maxine_fake_image.def)
    target_sources(maxine_fake_effects PRIVATE maxine_fake_effects.def)
    set_target_properties(maxine_fake_image PROPERTIES OUTPUT_NAME NVCVImage)
    set_target_properties(maxine_fake_effects PROPERTIES OUTPUT_NAME NVVideoEffects)
    add_executable(maxine_lifecycle_tests maxine_lifecycle_tests.cpp
        "${PROJECT_SOURCE_DIR}/src/common/maxine_superres.cpp")
    target_include_directories(maxine_lifecycle_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${maxine_fixture_include}")
    target_compile_definitions(maxine_lifecycle_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN OKUFLOW_ENABLE_CUDA=1
        OKUFLOW_MAXINE_FAKE_IMAGE="$<TARGET_FILE:maxine_fake_image>"
        OKUFLOW_MAXINE_FAKE_EFFECTS="$<TARGET_FILE:maxine_fake_effects>")
    target_compile_options(maxine_lifecycle_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(maxine_lifecycle_tests PRIVATE Qt6::Core CUDA::cudart_static advapi32)
    add_dependencies(maxine_lifecycle_tests maxine_fake_image maxine_fake_effects)
    add_test(NAME maxine_lifecycle_cuda COMMAND maxine_lifecycle_tests)
    set_tests_properties(maxine_lifecycle_cuda PROPERTIES
        TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS gpu
        REQUIRED_FILES "$<TARGET_FILE:maxine_fake_image>;$<TARGET_FILE:maxine_fake_effects>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

    add_executable(maxine_navigation_tests maxine_navigation_tests.cpp
        "${PROJECT_SOURCE_DIR}/src/common/maxine_async_producer.cpp"
        "${PROJECT_SOURCE_DIR}/src/cuda/cuda_interop.cpp"
        "${PROJECT_SOURCE_DIR}/src/cuda/uct_analysis_pool.cpp"
        "${PROJECT_SOURCE_DIR}/src/cuda/uct_kernels.cu"
        "${PROJECT_SOURCE_DIR}/src/cuda/cuda_kernels.cu"
        "${PROJECT_SOURCE_DIR}/src/cuda/spatial_upscalers.cu"
        "${PROJECT_SOURCE_DIR}/src/common/maxine_superres.cpp"
        "${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp")
    target_include_directories(maxine_navigation_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${maxine_fixture_include}")
    target_compile_definitions(maxine_navigation_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN OKUFLOW_ENABLE_CUDA=1
        OKUFLOW_HAS_CUDA_EXT_MEMORY=1 OKUFLOW_ENABLE_TEXT_SR=1
        OKUFLOW_MAXINE_FAKE_IMAGE="$<TARGET_FILE:maxine_fake_image>"
        OKUFLOW_MAXINE_FAKE_EFFECTS="$<TARGET_FILE:maxine_fake_effects>")
    target_compile_options(maxine_navigation_tests PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
    target_link_libraries(maxine_navigation_tests PRIVATE
        Qt6::Core Qt6::Gui CUDA::cudart_static CUDA::cuda_driver d3d11 d3d12 dxgi advapi32)
    set_target_properties(maxine_navigation_tests PROPERTIES
        CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static
        CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}")
    add_dependencies(maxine_navigation_tests maxine_fake_image maxine_fake_effects)
    add_test(NAME maxine_navigation_cuda COMMAND maxine_navigation_tests)
    set_tests_properties(maxine_navigation_cuda PROPERTIES
        TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS gpu RUN_SERIAL TRUE
        RESOURCE_LOCK okuflow_maxine_fixture
        REQUIRED_FILES "$<TARGET_FILE:maxine_fake_image>;$<TARGET_FILE:maxine_fake_effects>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
    add_executable(maxine_async_tests maxine_async_tests.cpp
        "${PROJECT_SOURCE_DIR}/src/common/maxine_async_producer.cpp"
        "${PROJECT_SOURCE_DIR}/src/common/maxine_superres.cpp")
    target_include_directories(maxine_async_tests PRIVATE "${PROJECT_SOURCE_DIR}/include" "${maxine_fixture_include}")
    target_compile_definitions(maxine_async_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN OKUFLOW_ENABLE_CUDA=1
        OKUFLOW_MAXINE_FAKE_IMAGE="$<TARGET_FILE:maxine_fake_image>"
        OKUFLOW_MAXINE_FAKE_EFFECTS="$<TARGET_FILE:maxine_fake_effects>")
    target_compile_options(maxine_async_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(maxine_async_tests PRIVATE Qt6::Core CUDA::cudart_static advapi32)
    add_dependencies(maxine_async_tests maxine_fake_image maxine_fake_effects)
    add_test(NAME maxine_async_cuda COMMAND maxine_async_tests)
    set_tests_properties(maxine_async_cuda PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS gpu RUN_SERIAL TRUE
        RESOURCE_LOCK okuflow_maxine_fixture
        REQUIRED_FILES "$<TARGET_FILE:maxine_fake_image>;$<TARGET_FILE:maxine_fake_effects>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
    set_property(TEST maxine_lifecycle_cuda APPEND PROPERTY RESOURCE_LOCK okuflow_maxine_fixture)
endif()
