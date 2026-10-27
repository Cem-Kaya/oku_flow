#pragma once

#ifdef _WIN32
#include <windows.h>
#include <cstdlib>

namespace okuflow {

// Call only after application RAII, owned-child shutdown, persisted writes
// and log flushing have completed. CUDA/TensorRT cleanup can remain detached
// or quarantined, so CRT/DLL teardown must not unload modules underneath it.
// This targets this process alone; the original application status is kept.
[[noreturn]] inline void FinishWindowsApplication(int exitCode) noexcept {
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
    // A self process handle always permits termination. If Windows refuses,
    // fail visibly instead of entering the unsafe global-destructor path.
    std::abort();
}

} // namespace okuflow
#endif
