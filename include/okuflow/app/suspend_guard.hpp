#pragma once

namespace okuflow {

class SuspendGuard final {
public:
    explicit SuspendGuard(bool& suspended)
        : suspended_(suspended)
    {
        suspended_ = true;
    }

    ~SuspendGuard() { suspended_ = false; }

    SuspendGuard(const SuspendGuard&) = delete;
    SuspendGuard& operator=(const SuspendGuard&) = delete;
    SuspendGuard(SuspendGuard&&) = delete;
    SuspendGuard& operator=(SuspendGuard&&) = delete;

private:
    bool& suspended_;
};

} // namespace okuflow
