#pragma once
#include <QImage>
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>

namespace okuflow::uct
{
// Immutable independently owned pre-zoom BGRA analysis image. Native handles
// are CUDA device pointers/events only in a CUDA build. No borrowed camera or
// presenter allocation survives submission. Readback is worker-only.
class DeviceImage : public std::enable_shared_from_this<DeviceImage>
{
  public:
    virtual ~DeviceImage() = default;
    int width{}, height{}, device{};
    std::size_t pitch{};
    std::uint64_t sourceEpoch{}, frameSequence{}, processingGeneration{};
    std::int64_t timestampMs{};
    virtual const void *Data() const = 0;
    virtual bool WaitReady(void *consumerStream, QString &error) const = 0;
    virtual QImage Readback(const std::atomic<bool> &cancelled, QString &error) const = 0;
    // Independent bounded validation branch; never an inference input.
    virtual QImage ReadProbe(const std::atomic<bool> &cancelled, QString &error) const
    {
        return Readback(cancelled, error);
    }
};
using SharedDeviceImage = std::shared_ptr<const DeviceImage>;
} // namespace okuflow::uct
