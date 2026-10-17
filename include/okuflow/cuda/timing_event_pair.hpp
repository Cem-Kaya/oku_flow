#pragma once

#include <utility>

namespace okuflow {

// Build the timing pair privately, then adopt both handles together. A failed
// second allocation must not turn the next frame into a null-stop record. The
// callbacks are nonthrowing CUDA-style create/destroy operations; keeping the
// handle generic also permits deterministic allocation-failure tests off GPU.
template <typename Event, typename Create, typename Destroy>
bool EnsureTimingEventPair(Event& start, Event& stop,
                           Create&& create, Destroy&& destroy)
{
    if (start && stop) return true;
    Event nextStart{};
    Event nextStop{};
    if (!create(&nextStart) || !create(&nextStop)) {
        if (nextStop) destroy(nextStop);
        if (nextStart) destroy(nextStart);
        return false;
    }
    std::swap(start, nextStart);
    std::swap(stop, nextStop);
    if (nextStop) destroy(nextStop);
    if (nextStart) destroy(nextStart);
    return true;
}

} // namespace okuflow
