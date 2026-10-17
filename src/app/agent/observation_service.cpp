#include "okuflow/app/agent/observation_service.hpp"
#include <QBuffer>
#include <QJsonArray>
#include <QPointer>
#include <QUuid>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace okuflow::agent {
namespace {
qint64 MonotonicNow() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
QJsonObject StampJson(ControlStamp stamp) {
    return {{"sourceEpoch",static_cast<qint64>(stamp.sourceEpoch)},{"processingVersion",static_cast<qint64>(stamp.processingVersion)},
        {"viewVersion",static_cast<qint64>(stamp.viewVersion)},{"geometryEpoch",static_cast<qint64>(stamp.geometryEpoch)}};
}
}
ObservationService::ObservationService(QObject* parent, std::function<qint64()> clock)
    : QObject(parent), clock_(clock ? std::move(clock) : MonotonicNow) {
    pool_.setMaxThreadCount(2); pool_.setExpiryTimeout(30000);
    connect(&expiry_,&QTimer::timeout,this,[this] {
        std::vector<ControlCompletion> callbacks;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (clock_() >= it->deadline) { callbacks.push_back(std::move(it->completion)); it = pending_.erase(it); }
            else ++it;
        }
        for (auto& callback : callbacks) callback(ControlResult::Failure("timeout"));
    });
    expiry_.start(50);
}
ObservationService::~ObservationService() { Cancel(); pool_.waitForDone(); }
void ObservationService::Request(bool image, int maxLongSide, ControlStamp expected, ControlCompletion completion,
                                 bool requireKnownTemporal) {
    if (pending_.size() >= 4) { completion(ControlResult::Failure("busy",true)); return; }
    pending_.push_back({image,std::clamp(maxLongSide,384,1024),expected,std::move(completion),clock_()+3000,requireKnownTemporal});
}
bool ObservationService::WantsReadback(const ObservationFrame& frame) const {
    return std::any_of(pending_.begin(),pending_.end(),[&](const Pending& request) {
        return request.expected == frame.stamp && (!request.requireKnownTemporal || (frame.held && frame.converging));
    });
}
void ObservationService::Consume(const QImage& viewport, const ObservationFrame& frame) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->expected != frame.stamp || (it->requireKnownTemporal && (!frame.held || !frame.converging))) { ++it; continue; }
        const auto request = std::move(*it); it = pending_.erase(it);
        QPointer<ObservationService> self(this);
        const auto generation = generation_;
        const auto pixels = viewport.copy();
        const bool accepted = pool_.tryStart([self,request,pixels,frame,generation] {
            auto result = Encode(pixels,frame,request.image,request.maxLongSide);
            if (!self) return;
            QMetaObject::invokeMethod(self,[self,request,frame,generation,result = std::move(result)]() mutable {
                if (!self) return;
                if (self->generation_ != generation) { request.completion(ControlResult::Failure("canceled")); return; }
                if (result.content["ok"].toBool()) {
                    self->metrics_ = result.content["metrics"].toObject(); self->metricsStamp_ = frame.stamp;
                    self->metricsProcessedAt_ = frame.processedAt;
                    result.content["metrics"] = self->Metrics(frame.stamp);
                    if (request.image) {
                        const auto id = result.content["observationId"].toString();
                        self->mappings_[id] = {frame,QSize(result.content["width"].toInt(),result.content["height"].toInt())};
                        while (self->mappings_.size() > 16) self->mappings_.erase(self->mappings_.begin());
                    }
                }
                request.completion(std::move(result));
            },Qt::QueuedConnection);
        });
        if (!accepted) request.completion(ControlResult::Failure("busy",true));
    }
}
void ObservationService::Cancel(const QString& code) {
    ++generation_; mappings_.clear(); metrics_ = {};
    auto pending = std::move(pending_); pending_.clear();
    for (auto& request : pending) request.completion(ControlResult::Failure(code));
}
QJsonObject ObservationService::Metrics(ControlStamp current) const {
    auto metrics = metrics_;
    const auto now = clock_();
    const auto age = std::max<qint64>(0, now-metricsProcessedAt_);
    metrics["frameAgeMs"] = age;
    if (age > MaximumFreshFrameAgeMs) metrics["fps"] = 0.0;
    metrics["stale"] = metrics_.isEmpty() || metricsStamp_ != current
        || metricsProcessedAt_ <= 0 || metricsProcessedAt_ > now || age > MaximumFreshFrameAgeMs
        || metrics["held"].toBool();
    metrics["ok"] = true;
    return metrics;
}
std::optional<ObservationPoint> ObservationService::Resolve(const QJsonObject& point, ControlStamp current) const {
    if (point["space"] == "scene") {
        const auto x=point["x"].toDouble(),y=point["y"].toDouble();
        if(!std::isfinite(x) || !std::isfinite(y) || x<0 || x>1 || y<0 || y>1)return {};
        return ObservationPoint{float(x),float(y)};
    }
    const auto found = mappings_.find(point["observationId"].toString());
    if (found == mappings_.end()) return {};
    const auto& mapping = found->second;
    if (mapping.frame.stamp.sourceEpoch != current.sourceEpoch || mapping.frame.stamp.geometryEpoch != current.geometryEpoch
        || mapping.imageSize.width() <= 0 || mapping.imageSize.height() <= 0) return {};
    const auto& transform = mapping.frame.transform;
    if (!transform.valid || transform.destinationWidth <= 0 || transform.destinationHeight <= 0) return {};
    const double x = point["px"].toDouble()/mapping.imageSize.width();
    const double y = point["py"].toDouble()/mapping.imageSize.height();
    if(!std::isfinite(x) || !std::isfinite(y) || x<0 || x>1 || y<0 || y>1)return {};
    if (x < transform.destinationX || x > transform.destinationX+transform.destinationWidth
        || y < transform.destinationY || y > transform.destinationY+transform.destinationHeight) return {};
    return ObservationPoint{float(transform.sourceX+(x-transform.destinationX)/transform.destinationWidth*transform.sourceWidth),
        float(transform.sourceY+(y-transform.destinationY)/transform.destinationHeight*transform.sourceHeight)};
}
ControlResult ObservationService::Encode(const QImage& viewport, const ObservationFrame& frame, bool image, int maxLongSide) {
    if (viewport.isNull() || viewport.width() <= 0 || viewport.height() <= 0 || !frame.transform.valid
        || static_cast<quint64>(viewport.width())*viewport.height()*4 > 64*1024*1024) return ControlResult::Failure("unavailable");
    QImage sample = viewport.convertToFormat(QImage::Format_ARGB32);
    std::array<std::uint64_t,256> histogram{};
    double sum = 0; std::uint64_t count = 0;
    for (int y = 0; y < sample.height(); y += 4) {
        const auto* row = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
        for (int x = 0; x < sample.width(); x += 4) {
            const int luma = qGray(row[x]); ++histogram[luma]; sum += luma/255.0; ++count;
        }
    }
    auto percentile = [&](double fraction) {
        std::uint64_t accumulated = 0;
        for (std::size_t i = 0; i < histogram.size(); ++i) {
            accumulated += histogram[i]; if (accumulated >= std::max<std::uint64_t>(1,static_cast<std::uint64_t>(count*fraction))) return i/255.0;
        }
        return 1.0;
    };
    const double mean = count ? sum/count : 0;
    double lapSum = 0, lapSquares = 0; quint64 lapCount = 0;
    for (int y = 1; y < sample.height()-1; y += 4) {
        const auto* row = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
        const auto* up = reinterpret_cast<const QRgb*>(sample.constScanLine(y-1));
        const auto* down = reinterpret_cast<const QRgb*>(sample.constScanLine(y+1));
        for (int x = 1; x < sample.width()-1; x += 4) {
            const double lap = (qGray(up[x])+qGray(down[x])+qGray(row[x-1])+qGray(row[x+1])-4*qGray(row[x]))/255.0;
            lapSum += lap; lapSquares += lap*lap; ++lapCount;
        }
    }
    const double focus = lapCount ? std::max(0.0,lapSquares/lapCount-std::pow(lapSum/lapCount,2)) : 0;
    const double p10 = percentile(0.1), p90 = percentile(0.9);
    QJsonObject metrics{{"lumaMean",mean},{"lumaP10",p10},{"lumaP90",p90},{"contrastSpread",p90-p10},
        {"sceneClass",mean > 0.62 ? "paper" : mean < 0.34 ? "board" : "mixed"},{"sceneClassBasis","viewport_luma"},
        {"focusScore",focus},{"focusAcceptable",focus >= frame.focusThreshold},{"focusBasis","viewport_laplacian"},
        {"keystoneQuad",frame.keystoneQuad ? QJsonValue(*frame.keystoneQuad) : QJsonValue(QJsonValue::Null)},
        {"frameAgeMs",std::max<qint64>(0,MonotonicNow()-frame.processedAt)},{"fps",frame.fps},{"fpsBasis","processed_unique_frames"},
        {"held",frame.held ? QJsonValue(*frame.held) : QJsonValue(QJsonValue::Null)},
        {"converging",frame.converging ? QJsonValue(*frame.converging) : QJsonValue(QJsonValue::Null)},
        {"stale",frame.processedAt <= 0 || frame.processedAt > MonotonicNow()
            || MonotonicNow()-frame.processedAt > MaximumFreshFrameAgeMs || frame.held.value_or(false)}};
    if (!frame.diagnostics.isEmpty()) metrics["diagnostics"] = frame.diagnostics;
    QJsonObject result{{"ok",true},{"metrics",metrics},{"versions",StampJson(frame.stamp)},{"frameSequence",static_cast<qint64>(frame.sequence)}};
    if (!image) return {result,{}};
    const QString id = QStringLiteral("obs-")+QUuid::createUuid().toString(QUuid::Id128);
    int side = std::clamp(maxLongSide,384,1024);
    QByteArray jpeg; QSize size;
    while (side >= 384) {
        const auto resized = sample.scaled(side,side,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        for (int quality : {90,75,60,45,30}) {
            jpeg.clear(); QBuffer buffer(&jpeg); buffer.open(QIODevice::WriteOnly);
            if (!resized.save(&buffer,"JPG",quality)) return ControlResult::Failure("internal");
            if (jpeg.size() <= 192*1024) { size = resized.size(); break; }
        }
        if (size.isValid()) break;
        if (side == 384) break;
        side = std::max(384,static_cast<int>(side*0.8));
    }
    if (!size.isValid()) return ControlResult::Failure("budget_exhausted");
    result["observationId"] = id; result["width"] = size.width(); result["height"] = size.height();
    result["visibleSourceRect"] = QJsonObject{{"x",frame.transform.sourceX},{"y",frame.transform.sourceY},
        {"width",frame.transform.sourceWidth},{"height",frame.transform.sourceHeight}};
    result["destinationRect"] = QJsonObject{{"x",frame.transform.destinationX},{"y",frame.transform.destinationY},
        {"width",frame.transform.destinationWidth},{"height",frame.transform.destinationHeight}};
    return {result,std::move(jpeg),AuthorizedObservation{
        {},frame.stamp,0,frame.sequence,frame.processedAt}};
}
} // namespace okuflow::agent
