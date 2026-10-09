# Project-wide configuration options and defaults.

option(OPENZOOM_ENABLE_CUDA "Enable CUDA interop path" ON)
option(OPENZOOM_ENABLE_TESTS "Enable building tests" OFF)
option(OPENZOOM_ENABLE_TEXT_SR "Enable the runtime-loaded NVIDIA Video Effects SuperRes adapter" ON)

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
