# Ultra Clear Text fixtures. Both run desktop-free under Qt offscreen.
if(NOT WIN32)
    return()
endif()

# The C++/WinRT translation unit stays free of the Qt precompiled header in
# this directory too (source properties are directory scoped).
set_source_files_properties(${PROJECT_SOURCE_DIR}/src/common/uct_windows_ocr.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Core model/scheduler/controller with production overlay and panel widgets
# and scripted recognizers (slow/held OCR, coalescing, stale rejection).
add_executable(ultra_clear_text_tests
    ultra_clear_text_tests.cpp
    ultra_clear_text_test_support.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/app/ultra_clear_text_controller.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/ultra_clear_text.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ultra_clear_text_overlay.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/uct_screen_text_accessible.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ultra_clear_text_panel.hpp
    ${PROJECT_SOURCE_DIR}/src/app/ultra_clear_text_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/common/ultra_clear_text.cpp
    ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ultra_clear_text_overlay.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/uct_screen_text_accessible.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ultra_clear_text_panel.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp)
target_include_directories(ultra_clear_text_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(ultra_clear_text_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(ultra_clear_text_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
add_test(NAME ultra_clear_text_contract COMMAND ultra_clear_text_tests)
set_tests_properties(ultra_clear_text_contract PROPERTIES TIMEOUT 120 LABELS "headless"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

# Optional real-backend acceptance: built-in Windows.Media.Ocr, no models.
# Reports SKIP when Windows has no OCR-capable language; that is not a pass.
add_executable(ultra_clear_text_windows_ocr_tests
    ultra_clear_text_windows_ocr_tests.cpp
    ultra_clear_text_test_support.hpp
    ${PROJECT_SOURCE_DIR}/src/common/ultra_clear_text.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_default_recognizer.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_paddle.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_paddle_processing.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_onnx_cpu.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_tensorrt.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_windows_ocr.cpp
    ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)
target_include_directories(ultra_clear_text_windows_ocr_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
okuflow_configure_ocr(ultra_clear_text_windows_ocr_tests)
target_compile_definitions(ultra_clear_text_windows_ocr_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(ultra_clear_text_windows_ocr_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test windowsapp)
add_test(NAME ultra_clear_text_windows_ocr COMMAND ultra_clear_text_windows_ocr_tests)
set_tests_properties(ultra_clear_text_windows_ocr PROPERTIES TIMEOUT 60 LABELS "headless;ocr-runtime"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")

add_executable(uct_paddle_tests
    uct_paddle_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/common/ultra_clear_text.cpp
    ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_paddle_processing.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_paddle.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_onnx_cpu.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_tensorrt.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_default_recognizer.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_windows_ocr.cpp)
target_include_directories(uct_paddle_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(uct_paddle_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(uct_paddle_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test windowsapp)
okuflow_configure_ocr(uct_paddle_tests)
if(OKUFLOW_ENABLE_CUDA)
    target_sources(uct_paddle_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cuda/uct_analysis_pool.cpp
        ${PROJECT_SOURCE_DIR}/src/cuda/uct_kernels.cu)
    target_link_libraries(uct_paddle_tests PRIVATE CUDA::cudart_static)
    set_target_properties(uct_paddle_tests PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}")
    target_sources(ultra_clear_text_windows_ocr_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/cuda/uct_analysis_pool.cpp
        ${PROJECT_SOURCE_DIR}/src/cuda/uct_kernels.cu)
    target_link_libraries(ultra_clear_text_windows_ocr_tests PRIVATE CUDA::cudart_static)
    set_target_properties(ultra_clear_text_windows_ocr_tests PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}")
endif()
add_test(NAME uct_paddle_processing COMMAND uct_paddle_tests modelPinsMatchPreparationAndFailClosed normalizationAndBounds detectionRejectsNoiseAndMapsSource ctcBlankRepeatSpaceAndConfidence decoderPreservesUncertainNegationThroughReading automaticFallback allBackendsUnavailable missingOrTamperedModelsFailClosed copyFaultKeepsFallbackUsableAndCopiesOnce cpuServesWhileGpuPreparesThenGpuPromotes promotedGpuFailureDoesNotResurrectRetiredCpu analysisAllocationBackoffAndUnknownCompletionQuarantine)
set_tests_properties(uct_paddle_processing PROPERTIES TIMEOUT 30 LABELS "headless"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
if(OKUFLOW_ENABLE_CUDA)
    add_test(NAME uct_paddle_cuda COMMAND uct_paddle_tests gpuResidentSchedulerKeepsPixelsAndDescriptorsOnDevice gpuResidentFingerprintCoversEveryByteAndReleasesCaptureSlots gpuResidentValidationMatchesCpuUnderMotionOcclusionAndCrop gpuResidentBatchMappingMatchesDistinctCpuLines gpuAlgorithmsAndOwnedInputs gpuDenseRegionSelectionIsDeterministic gpuOrientedRegionsMatchCpuCornerOrder)
    set_tests_properties(uct_paddle_cuda PROPERTIES TIMEOUT 60 LABELS "gpu"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
endif()
