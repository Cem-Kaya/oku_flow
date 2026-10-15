#pragma once
#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/common/view_transform.hpp"
#include <QImage>
#include <QSize>
#include <QThreadPool>
#include <QTimer>

namespace okuflow::agent {
struct ObservationFrame {
    ControlStamp stamp;
    ViewTransform transform;
    QSize sourceSize, viewportSize;
    quint64 sequence{};
    qint64 processedAt{};
    std::optional<bool> held, converging;
    double fps{};
    float focusThreshold{0.012f};
    std::optional<bool> keystoneQuad;
    QJsonObject diagnostics;
};
struct ObservationPoint { float x{}, y{}; };

// Owns its consumer slot logically: request-id matching and the immutable
// present stamp are separate from photo/Assistant consumers of a GPU readback.
class ObservationService final : public QObject {
public:
    explicit ObservationService(QObject* parent = nullptr, std::function<qint64()> clock = {});
    static constexpr qint64 MaximumFreshFrameAgeMs = 1000;
    ~ObservationService() override;
    void Request(bool includeImage, int maxLongSide, ControlStamp expected, ControlCompletion completion,
                 bool requireKnownTemporal = false);
    bool WantsReadback(const ObservationFrame& frame) const;
    void Consume(const QImage& viewport, const ObservationFrame& frame);
    void Cancel(const QString& code = QStringLiteral("canceled"));
    QJsonObject Metrics(ControlStamp current) const;
    std::optional<ObservationPoint> Resolve(const QJsonObject& point, ControlStamp current) const;
    static ControlResult Encode(const QImage& viewport, const ObservationFrame& frame,
                                bool includeImage, int maxLongSide);
private:
    struct Pending { bool image; int maxLongSide; ControlStamp expected; ControlCompletion completion; qint64 deadline;
                     bool requireKnownTemporal; };
    struct Mapping { ObservationFrame frame; QSize imageSize; };
    std::deque<Pending> pending_;
    std::map<QString,Mapping> mappings_;
    QJsonObject metrics_;
    ControlStamp metricsStamp_;
    qint64 metricsProcessedAt_{};
    std::function<qint64()> clock_;
    QThreadPool pool_;
    QTimer expiry_;
    quint64 generation_{};
};
} // namespace okuflow::agent
