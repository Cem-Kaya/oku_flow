#include "okuflow/common/uct_paddle.hpp"

#include <QLineF>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace okuflow::uct
{
namespace
{
class PaddleCpuRecognizer final : public Recognizer
{
  public:
    explicit PaddleCpuRecognizer(QString root) : root_(std::move(root))
    {
    }
    BackendStatus Initialize() override
    {
        status_.name = "PaddleOCR / ONNX CPU";
        status_.languageTag = "multilingual";
        status_.state = BackendState::kUnavailable;
        assets_ = LoadPaddleAssets(root_);
        if (!assets_.IsValid())
        {
            status_.detail = assets_.error;
            return status_;
        }
        try
        {
            const auto runtime = QDir(root_).filePath("cpu/onnxruntime.dll");
            detector_ = OpenPaddleCpuSession(runtime, assets_.detector);
            recognizer_ = OpenPaddleCpuSession(runtime, assets_.recognizer);
            status_.state = BackendState::kReady;
            status_.detail = "PP-OCRv6 tiny detector / small recognizer; CPU fallback.";
        }
        catch (const std::exception &e)
        {
            status_.detail = QString::fromUtf8(e.what());
        }
        return status_;
    }
    BackendStatus Status() const override
    {
        return status_;
    }
    RecognitionOutput Recognize(const QImage &image, const std::atomic<bool> &cancelled) override
    {
        RecognitionOutput result;
        try
        {
            if (cancelled.load())
            {
                result.cancelled = true;
                return result;
            }
            if (!detector_ || !recognizer_ || image.isNull())
                throw std::runtime_error("PaddleOCR is not ready.");
            const QSize size = PaddleDetectionSize(image.size());
            const auto pixels = PaddleNormalize(image, size, true);
            const std::array<std::int64_t, 4> shape{1, 3, size.height(), size.width()};
            const auto map = detector_->Run(pixels, shape);
            if (map.shape.size() != 4 || map.shape[0] != 1 || map.shape[1] != 1 ||
                map.shape[2] <= 0 || map.shape[3] <= 0)
                throw std::runtime_error("Unexpected detector output dimensions.");
            const auto quads =
                PaddleDetect(map.data, static_cast<int>(map.shape[3]),
                             static_cast<int>(map.shape[2]), image.size(), cancelled);
            for (const auto &quad : quads)
            {
                if (cancelled.load())
                {
                    result.cancelled = true;
                    result.lines.clear();
                    return result;
                }
                const double w = QLineF(quad.points[0], quad.points[1]).length();
                const double h = QLineF(quad.points[0], quad.points[3]).length();
                if (w < 2 || h < 2)
                    continue;
                constexpr int width = 1024;
                const int validWidth =
                    std::clamp(static_cast<int>(std::ceil(48 * w / h)), 1, width);
                const auto normalized = PaddleRectify(image, quad, width, validWidth);
                const std::array<std::int64_t, 4> recShape{1, 3, 48, width};
                const auto logits = recognizer_->Run(normalized, recShape);
                if (logits.shape.size() != 3 || logits.shape[0] != 1 || logits.shape[1] <= 0 ||
                    logits.shape[2] != assets_.dictionary.size())
                    throw std::runtime_error("PaddleOCR dictionary/output dimension mismatch.");
                const int steps = static_cast<int>(logits.shape[1]),
                          classes = static_cast<int>(logits.shape[2]);
                const int validSteps = std::min(
                    steps,
                    static_cast<int>(std::ceil(steps * validWidth / static_cast<double>(width))));
                auto line = PaddleDecode(
                    std::span(logits.data).first(static_cast<std::size_t>(validSteps) * classes),
                    validSteps, classes, quad, assets_.dictionary);
                if (!line.words.isEmpty())
                    result.lines.push_back(std::move(line));
            }
            result.ok = !cancelled.load();
            result.cancelled = cancelled.load();
        }
        catch (const std::exception &e)
        {
            result.error = QString::fromUtf8(e.what());
        }
        return result;
    }

  private:
    QString root_;
    PaddleAssets assets_;
    BackendStatus status_;
    std::unique_ptr<PaddleCpuSession> detector_, recognizer_;
};

class AutomaticRecognizer final : public Recognizer
{
  public:
    explicit AutomaticRecognizer(std::vector<std::unique_ptr<Recognizer>> candidates = {})
        : candidates_(std::move(candidates))
    {
    }
    BackendStatus Initialize() override
    {
        if (initialized_)
            return status_;
        initialized_ = true;
        if (candidates_.empty())
        {
            candidates_.push_back(CreateTensorRtRecognizer());
            candidates_.push_back(CreatePaddleCpuRecognizer());
            candidates_.push_back(CreateWindowsRecognizer());
        }
        states_.resize(candidates_.size(), CandidateState::kUninitialized);
        Advance();
        return status_;
    }
    BackendStatus Status() const override
    {
        return status_;
    }
    bool WantsDeviceInput() const override
    {
        if (preparing_ && !candidates_[*preparing_]->Preparing() &&
            candidates_[*preparing_]->Status().state == BackendState::kReady)
            return candidates_[*preparing_]->WantsDeviceInput();
        return active_ < candidates_.size() && candidates_[active_] &&
               candidates_[active_]->WantsDeviceInput();
    }
    RecognitionOutput RecognizeDevice(const DeviceImage &image,
                                      const std::atomic<bool> &cancelled) override
    {
        QImage pixels;
        bool copied = false;
        return Run(
            [&](Recognizer &backend) {
                if (backend.WantsDeviceInput())
                    return backend.RecognizeDevice(image, cancelled);
                RecognitionOutput output;
                if (!copied)
                {
                    copied = true;
                    try
                    {
                        pixels = image.Readback(cancelled, output.error);
                    }
                    catch (const std::exception &error)
                    {
                        output.error = QString::fromUtf8(error.what());
                    }
                }
                if (pixels.isNull())
                {
                    output.inputFault = true;
                    output.cancelled = cancelled.load();
                    return output;
                }
                return backend.Recognize(pixels, cancelled);
            },
            cancelled);
    }
    RecognitionOutput Recognize(const QImage &image, const std::atomic<bool> &cancelled) override
    {
        return Run([&](Recognizer &backend) { return backend.Recognize(image, cancelled); },
                   cancelled);
    }

  private:
    RecognitionOutput Run(const std::function<RecognitionOutput(Recognizer &)> &operation,
                          const std::atomic<bool> &cancelled)
    {
        RecognitionOutput output;
        if (preparing_ && !candidates_[*preparing_]->Preparing())
        {
            const auto preferred = *preparing_;
            preparing_.reset();
            const auto preferredStatus = candidates_[preferred]->Status();
            if (preferredStatus.state == BackendState::kReady)
                active_ = preferred;
            else
            {
                states_[preferred] = CandidateState::kRetired;
                reasons_ += preferredStatus.name + ": " + preferredStatus.detail + "; ";
                if (active_ >= candidates_.size())
                    Advance();
            }
        }
        while (active_ < candidates_.size())
        {
            if (cancelled.load())
            {
                output.cancelled = true;
                return output;
            }
            if (candidates_[active_]->Preparing())
            {
                preparing_ = active_;
                ++active_;
                Advance();
                continue;
            }
            output = {};
            try
            {
                output = operation(*candidates_[active_]);
            }
            catch (const std::exception &error)
            {
                output.error = QString::fromUtf8(error.what());
            }
            if (output.backendPreparing)
            {
                preparing_ = active_;
                ++active_;
                Advance();
                continue;
            }
            if (output.ok || output.cancelled || output.inputFault || cancelled.load())
            {
                const auto current = candidates_[active_]->Status();
                if (current.state == BackendState::kReady)
                {
                    status_ = current;
                    status_.detail = reasons_ + status_.detail;
                }
                return output;
            }
            reasons_ += candidates_[active_]->Status().name + ": " + output.error + "; ";
            states_[active_] = CandidateState::kRetired;
            ++active_;
            Advance();
        }
        output.error = status_.detail;
        output.backendPreparing = preparing_.has_value();
        return output;
    }
    void Advance()
    {
        while (active_ < candidates_.size())
        {
            if (candidates_[active_] && states_[active_] != CandidateState::kRetired)
            {
                try
                {
                    status_ = states_[active_] == CandidateState::kReady
                                  ? candidates_[active_]->Status()
                                  : candidates_[active_]->Initialize();
                }
                catch (const std::exception &error)
                {
                    status_ = {};
                    status_.state = BackendState::kUnavailable;
                    status_.detail = QString::fromUtf8(error.what());
                }
                if (status_.state == BackendState::kReady)
                {
                    states_[active_] = CandidateState::kReady;
                    status_.detail = reasons_ + status_.detail;
                    return;
                }
                reasons_ += status_.name + ": " + status_.detail + "; ";
            }
            states_[active_] = CandidateState::kRetired;
            ++active_;
        }
        if (preparing_ && candidates_[*preparing_]->Preparing())
        {
            status_ = candidates_[*preparing_]->Status();
            return;
        }
        status_.state = BackendState::kUnavailable;
        status_.detail = reasons_;
    }
    enum class CandidateState
    {
        kUninitialized,
        kReady,
        kRetired
    };
    std::vector<std::unique_ptr<Recognizer>> candidates_;
    std::vector<CandidateState> states_;
    std::size_t active_{};
    QString reasons_;
    BackendStatus status_;
    bool initialized_{};
    std::optional<std::size_t> preparing_;
};
} // namespace
std::unique_ptr<Recognizer> CreatePaddleCpuRecognizer(const QString &root)
{
    return std::make_unique<PaddleCpuRecognizer>(root.isEmpty() ? DefaultPaddleRoot() : root);
}
std::unique_ptr<Recognizer> CreateAutomaticRecognizer()
{
    return std::make_unique<AutomaticRecognizer>();
}
std::unique_ptr<Recognizer> CreateAutomaticRecognizer(
    std::vector<std::unique_ptr<Recognizer>> candidates)
{
    return std::make_unique<AutomaticRecognizer>(std::move(candidates));
}
} // namespace okuflow::uct
