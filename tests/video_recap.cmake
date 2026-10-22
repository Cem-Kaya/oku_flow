if(WIN32)
    target_sources(media_writer_gpu_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/common/video_recap.cpp)
    add_test(NAME video_recap_native_gpu COMMAND media_writer_gpu_tests --ram-recap)
    set_tests_properties(video_recap_native_gpu PROPERTIES TIMEOUT 60 LABELS "codec;gpu" RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77 REQUIRED_FILES
        "${PROJECT_SOURCE_DIR}/tests/media_writer_gpu_tests.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/media_writer.hpp;${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/video_recap.hpp;${PROJECT_SOURCE_DIR}/src/common/video_recap.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/video_memory_stream.hpp;${PROJECT_SOURCE_DIR}/src/common/video_memory_stream.cpp")
    add_executable(video_recap_tests video_recap_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_recap.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_memory_stream.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_playback.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_ffmpeg_reader.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_gpu_decoder.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_archive.cpp
        ${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp
        ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp)
    target_sources(video_recap_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)
    target_include_directories(video_recap_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(video_recap_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(video_recap_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(video_recap_tests PRIVATE mfplat mfreadwrite mfuuid propsys ole32 oleaut32 d3d11 dxgi)
    okuflow_configure_ffmpeg_playback(video_recap_tests)
    add_test(NAME video_recap_contract COMMAND video_recap_tests)
    add_test(NAME video_recap_native_codec COMMAND video_recap_tests --native-codec)
    add_test(NAME video_recap_native_4k30 COMMAND video_recap_tests --native-4k30)
    set_tests_properties(video_recap_contract PROPERTIES TIMEOUT 30 LABELS "headless")
    set_tests_properties(video_recap_native_codec video_recap_native_4k30 PROPERTIES TIMEOUT 60 LABELS "codec" RUN_SERIAL TRUE)
    set_tests_properties(video_recap_contract video_recap_native_codec video_recap_native_4k30 PROPERTIES REQUIRED_FILES
        "${PROJECT_SOURCE_DIR}/include/okuflow/app/frozen_camera_scene.hpp;${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/video_recap.hpp;${PROJECT_SOURCE_DIR}/src/common/video_recap.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/video_memory_stream.hpp;${PROJECT_SOURCE_DIR}/src/common/video_memory_stream.cpp;${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp;${PROJECT_SOURCE_DIR}/src/common/video_playback.cpp")
    add_executable(video_memory_stream_tests video_memory_stream_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_memory_stream.cpp
        ${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp
        ${PROJECT_SOURCE_DIR}/src/common/video_archive.cpp
        ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp)
    target_include_directories(video_memory_stream_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(video_memory_stream_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(video_memory_stream_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(video_memory_stream_tests PRIVATE mfplat mfreadwrite mfuuid propsys ole32 oleaut32 d3d11 dxgi)
    add_test(NAME video_memory_stream_contract COMMAND video_memory_stream_tests)
    add_test(NAME video_memory_native_h264 COMMAND video_memory_stream_tests --native-h264)
    set_tests_properties(video_memory_native_h264 PROPERTIES TIMEOUT 60 LABELS "codec" RUN_SERIAL TRUE)
    set_tests_properties(video_memory_native_h264 PROPERTIES REQUIRED_FILES
        "${PROJECT_SOURCE_DIR}/include/okuflow/common/video_memory_stream.hpp;${PROJECT_SOURCE_DIR}/src/common/video_memory_stream.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/media_writer.hpp;${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp")
    set_tests_properties(video_memory_stream_contract PROPERTIES TIMEOUT 30 LABELS "headless" REQUIRED_FILES
        "${PROJECT_SOURCE_DIR}/include/okuflow/common/video_memory_stream.hpp;${PROJECT_SOURCE_DIR}/src/common/video_memory_stream.cpp")
endif()
