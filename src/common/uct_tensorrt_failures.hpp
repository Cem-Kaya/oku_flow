#pragma once

#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace okuflow::uct::detail {

class CudaFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class TensorRtApiFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Shared by the production factory checks and the model-free failure fixture.
template<class T> T* RequireTensorRtApi(T* value, std::string_view message) {
    if (!value) throw TensorRtApiFailure(std::string(message));
    return value;
}

// A cache/profile/capacity refusal may select a smaller prepared engine.
// API, CUDA and allocation faults must leave preparation with their type intact.
inline bool AllowsTensorRtCapacityFallback(const std::exception_ptr& failure) {
    if (!failure) return false;
    try { std::rethrow_exception(failure); }
    catch (const CudaFailure&) { return false; }
    catch (const TensorRtApiFailure&) { return false; }
    catch (const std::bad_alloc&) { return false; }
    catch (const std::exception&) { return true; }
    catch (...) { return false; }
}

} // namespace okuflow::uct::detail
