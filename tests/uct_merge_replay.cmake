# Model-free CPU replay. Keep it independent of the GPU/batch fixture TU.
add_executable(uct_merge_replay_tests
    uct_merge_replay.cpp
    uct_merge_baseline_bb53.hpp
    uct_merge_baseline_bb53.cpp
    uct_merge_baseline_bb53_geometry.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_paddle_processing.cpp
    ${PROJECT_SOURCE_DIR}/src/common/ultra_clear_text.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_region_planner.cpp
    ${PROJECT_SOURCE_DIR}/src/common/uct_timing_trace.cpp
    ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)
target_include_directories(uct_merge_replay_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(uct_merge_replay_tests PRIVATE
    OKUFLOW_ENABLE_CUDA=0 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(uct_merge_replay_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(uct_merge_replay_tests PRIVATE Qt6::Core Qt6::Gui)
set_target_properties(uct_merge_replay_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF DISABLE_PRECOMPILE_HEADERS ON)
add_test(NAME uct_merge_replay COMMAND uct_merge_replay_tests)
set_tests_properties(uct_merge_replay PROPERTIES TIMEOUT 60 LABELS "headless"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    REQUIRED_FILES "${CMAKE_CURRENT_SOURCE_DIR}/uct_merge_replay.cpp;${CMAKE_CURRENT_SOURCE_DIR}/uct_merge_baseline_bb53.hpp;${CMAKE_CURRENT_SOURCE_DIR}/uct_merge_baseline_bb53.cpp;${CMAKE_CURRENT_SOURCE_DIR}/uct_merge_baseline_bb53_geometry.cpp;${PROJECT_SOURCE_DIR}/src/common/uct_paddle_processing.cpp;${PROJECT_SOURCE_DIR}/src/common/ultra_clear_text.cpp")
# --benchmark is explicit only; ordinary CTest never runs the timing blocks.
