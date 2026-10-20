if(WIN32)
    add_executable(video_playback_tests
        video_playback_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_playback.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_gpu_decoder.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_archive.cpp
        ${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp
        ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp)
    target_include_directories(video_playback_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(video_playback_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(video_playback_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(video_playback_tests PRIVATE mfplat mfreadwrite mfuuid propsys ole32 d3d11 dxgi)
    add_test(NAME video_playback_contract COMMAND video_playback_tests)
    add_test(NAME video_archive_native_avc_aac COMMAND video_playback_tests --native-avc)
    add_test(NAME video_archive_native_av1_aac COMMAND video_playback_tests --native-av1)
    add_test(NAME video_playback_native_4k COMMAND video_playback_tests --native-4k)
    add_test(NAME video_playback_native_dci_4k COMMAND video_playback_tests --native-dci-4k)
    add_test(NAME video_playback_gpu_avc COMMAND video_playback_tests --native-gpu-avc)
    add_test(NAME video_playback_gpu_av1 COMMAND video_playback_tests --native-gpu-av1)
    add_test(NAME video_playback_gpu_4k COMMAND video_playback_tests --native-gpu-4k)
    add_test(NAME video_playback_gpu_dci_4k COMMAND video_playback_tests --native-gpu-dci-4k)
    add_test(NAME video_gpu_conversion COMMAND video_playback_tests --gpu-conversion)
    set_tests_properties(video_playback_gpu_avc video_playback_gpu_av1 video_playback_gpu_4k video_playback_gpu_dci_4k video_gpu_conversion PROPERTIES
        TIMEOUT 120 LABELS "gpu;codec" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
        REQUIRED_FILES "${PROJECT_SOURCE_DIR}/src/common/video_gpu_decoder.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/video_gpu_decoder.hpp")
    set_tests_properties(video_archive_native_avc_aac video_archive_native_av1_aac video_playback_native_4k video_playback_native_dci_4k
        PROPERTIES TIMEOUT 120 LABELS "codec" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
        REQUIRED_FILES "${PROJECT_SOURCE_DIR}/src/common/video_playback.cpp;${PROJECT_SOURCE_DIR}/src/common/video_archive.cpp;${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp")
    set_tests_properties(video_playback_contract PROPERTIES TIMEOUT 60 LABELS "codec"
        REQUIRED_FILES "${PROJECT_SOURCE_DIR}/src/common/video_playback.cpp;${PROJECT_SOURCE_DIR}/src/common/video_archive.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/video_playback.hpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/video_archive.hpp;${PROJECT_SOURCE_DIR}/tests/data/long-gop-h264.mp4;${PROJECT_SOURCE_DIR}/tests/data/uct-scene-cut-h264.mp4")
    # Explicit private fixtures permit wider codec/profile acceptance without
    # adding a downloaded sample or an encoder dependency to the application.
    set(OKUFLOW_TEST_VIDEO_GPU_FILES "" CACHE STRING "Absolute private video files for optional native GPU acceptance")
    foreach(video IN LISTS OKUFLOW_TEST_VIDEO_GPU_FILES)
        if(NOT IS_ABSOLUTE "${video}" OR NOT EXISTS "${video}")
            message(FATAL_ERROR "Private GPU video fixture must be an existing absolute file: ${video}")
        endif()
        get_filename_component(stem "${video}" NAME_WE)
        string(MAKE_C_IDENTIFIER "${stem}" fixture)
        add_test(NAME "video_gpu_format_${fixture}" COMMAND video_playback_tests --native-gpu-external "${video}")
        set_tests_properties("video_gpu_format_${fixture}" PROPERTIES TIMEOUT 60 LABELS "gpu;codec"
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 REQUIRED_FILES "${video}")
    endforeach()
endif()
