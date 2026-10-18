if(WIN32)
    add_executable(video_gpu_snapshot_tests video_gpu_snapshot_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/d3d12/presenter.cpp
        ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)
    target_include_directories(video_gpu_snapshot_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(video_gpu_snapshot_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN OKUFLOW_ENABLE_CUDA=0)
    target_compile_options(video_gpu_snapshot_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(video_gpu_snapshot_tests PRIVATE Qt6::Core Qt6::Gui d3d12 dxgi d3dcompiler)
    add_test(NAME video_gpu_snapshot COMMAND video_gpu_snapshot_tests)
    set_tests_properties(video_gpu_snapshot PROPERTIES TIMEOUT 30 LABELS gpu RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
endif()
