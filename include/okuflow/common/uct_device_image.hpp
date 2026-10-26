#pragma once
#include <QImage>
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>

namespace okuflow::uct
{
struct RecognitionResult;
// Provenance of the OCR (Data) half of an analysis image. SingleFrame is
// exactly frameSequence. TemporallySmoothed is a neutral post-geometry EMA
// whose latest contributing frame is frameSequence; the blend fields below
// disclose its history. The validation half is always the single exact
// frameSequence frame, never smoothed.
enum class AnalysisKind : std::uint8_t
{
    SingleFrame,
    TemporallySmoothed
};
// Extra Stable (bump hold) identity of the copied, displayed pixels. NotHeld:
// no hold policy ran, so the pixels are frameSequence. Live: the policy ran
// and showed frameSequence itself. Held: an earlier acquisition retained by
// the hold surface. Crossfade: a recovery blend of held and live pixels.
// Unknown: the device record was missing, inconsistent or unreadable.
enum class HoldDisplay : std::uint8_t
{
    NotHeld,
    Live,
    Held,
    Crossfade,
    Unknown
};
struct HoldProvenance
{
    HoldDisplay display{HoldDisplay::NotHeld};
    std::uint64_t acquisition{}; // frame whose pixels are shown (Live/Held only)
    float blendAlpha{};          // live weight of a Crossfade
};
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
    AnalysisKind kind{AnalysisKind::SingleFrame};
    std::uint64_t historyEpoch{};      // UCT EMA generation; changes on every reseed
    std::uint64_t seedFrameSequence{}; // first contributing frame of this history
    std::uint64_t blendFrameCount{};   // frames folded into Data (1 == seed only)
    float temporalAlpha{};             // EMA alpha of Data; 0 when SingleFrame
    virtual const void *Data() const = 0;
    virtual bool WaitReady(void *consumerStream, QString &error) const = 0;
    // Single exact current-frame pixels paired with Data. Probes, reference
    // patches and source fingerprints must use these, never Data, so a
    // smoothed ghost can never validate itself. Defaults alias Data.
    virtual const void *ValidationData() const { return Data(); }
    virtual std::size_t ValidationPitch() const { return pitch; }
    virtual bool WaitValidationReady(void *consumerStream, QString &error) const
    {
        return WaitReady(consumerStream, error);
    }
    // Production CUDA images provide worker-only analysis without pixel egress.
    // False means an explicit CPU image/fallback implementation, never a silent
    // retry after a resident-analysis failure.
    virtual bool HasResidentAnalysis() const { return false; }
    virtual bool PrepareResidentAnalysis(RecognitionResult&, int, bool,
                                         const std::atomic<bool>&, QString&) const { return false; }
    // Optional contour-only extraction after final recognition geometry.
    // Missing implementations leave protected ink original; never download it.
    virtual bool PrepareResidentInkOutlines(RecognitionResult&,const std::atomic<bool>&,QString&) const { return true; }
    virtual QImage Readback(const std::atomic<bool> &cancelled, QString &error) const = 0;
    // Full-resolution validation pixels, for explicit CPU fallback reference
    // and fingerprint work only. Defaults to the OCR pixels.
    virtual QImage ReadValidation(const std::atomic<bool> &cancelled, QString &error) const
    {
        return Readback(cancelled, error);
    }
    // Independent bounded validation branch; never an inference input.
    virtual QImage ReadProbe(const std::atomic<bool> &cancelled, QString &error) const
    {
        return Readback(cancelled, error);
    }
    // True when the capture copied a device-resident Extra Stable record with
    // its pixels. Until a worker reads it, the image names no acquisition.
    virtual bool HasHoldProvenance() const { return false; }
    // Worker-only: waits (bounded, cancellable) for this capture's own copy,
    // then returns the compact record. Never a pixel readback or UI wait.
    virtual HoldProvenance ReadHoldProvenance(const std::atomic<bool> &cancelled,
                                              QString &error) const
    {
        (void)cancelled;
        error = QStringLiteral("This analysis image has no Extra Stable record.");
        return {HoldDisplay::Unknown, 0, 0.0f};
    }
};
using SharedDeviceImage = std::shared_ptr<const DeviceImage>;
} // namespace okuflow::uct
