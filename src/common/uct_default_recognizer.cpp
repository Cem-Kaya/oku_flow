#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/common/uct_paddle.hpp"

#if defined(_WIN32)
#include "okuflow/common/uct_windows_ocr.hpp"
#include "uct_windows_geometry.hpp"
#endif

#include <QImage>

#include <algorithm>

// Kept apart from the core so fixtures can link UCT logic without WinRT.
namespace okuflow::uct {

#if defined(_WIN32)
namespace {

int Luma(const uchar* bgra)
{
    return (29 * bgra[0] + 150 * bgra[1] + 77 * bgra[2]) >> 8;
}

class WindowsOcrRecognizer final : public Recognizer {
public:
    BackendStatus Initialize() override
    {
        auto& status = status_;
        status.name = QStringLiteral("Windows.Media.Ocr");
        const windows_ocr::InitResult init = engine_.Initialize();
        status.languageTag = QString::fromStdWString(init.languageTag);
        status.detail = QString::fromStdString(init.detail);
        status.state = init.ready ? BackendState::kReady : BackendState::kUnavailable;
        maximumDimension_ = init.maxImageDimension > 0 ? static_cast<int>(init.maxImageDimension) : 2600;
        return status;
    }

    BackendStatus Status() const override { return status_; }

    RecognitionOutput Recognize(const QImage& image, const std::atomic<bool>& cancelled) override
    {
        RecognitionOutput output;
        if (image.isNull()) {
            output.error = QStringLiteral("Empty image.");
            return output;
        }
        // Grayscale with dark-background inversion: the recognizer is tuned
        // for dark print on a light page, while users often read inverted.
        const double scale = std::min(1.0, static_cast<double>(maximumDimension_) /
                                               std::max(image.width(), image.height()));
        QImage source = image;
        if (scale < 1.0) {
            source = image.scaled(std::max(1, static_cast<int>(image.width() * scale)),
                                  std::max(1, static_cast<int>(image.height() * scale)),
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        source = source.convertToFormat(QImage::Format_ARGB32);
        long long sum = 0;
        for (int y = 0; y < source.height(); y += 4) {
            const uchar* row = source.constScanLine(y);
            for (int x = 0; x < source.width(); x += 4) {
                sum += Luma(row + x * 4);
            }
        }
        const long long samples =
            static_cast<long long>((source.height() + 3) / 4) * ((source.width() + 3) / 4);
        const bool invert = samples > 0 && sum / samples < 110;
        QImage prepared(source.size(), QImage::Format_ARGB32);
        for (int y = 0; y < source.height(); ++y) {
            const uchar* in = source.constScanLine(y);
            uchar* out = prepared.scanLine(y);
            for (int x = 0; x < source.width(); ++x) {
                int l = Luma(in + x * 4);
                if (invert) {
                    l = 255 - l;
                }
                out[x * 4 + 0] = out[x * 4 + 1] = out[x * 4 + 2] = static_cast<uchar>(l);
                out[x * 4 + 3] = 255;
            }
        }
        const windows_ocr::Result r =
            engine_.Recognize(prepared.constBits(), prepared.width(), prepared.height(),
                              static_cast<int>(prepared.bytesPerLine()), cancelled);
        output.ok = r.ok;
        output.cancelled = r.cancelled;
        output.error = QString::fromStdString(r.error);
        for (const windows_ocr::Line& line : r.lines) {
            RecognizedLine out;
            for (const windows_ocr::Word& word : line.words) {
                const auto quad = windows_ocr::SourceWordQuad(word, prepared.size(), image.size(),
                                                            r.textAngleDegrees);
                if (!quad.valid) {
                    output.ok = false;
                    output.lines.clear();
                    output.error = QStringLiteral("Windows OCR returned invalid word geometry.");
                    return output;
                }
                RecognizedWord mapped{QString::fromStdWString(word.text), quad.Bounds()};
                mapped.quadPx = quad;
                out.words.push_back(std::move(mapped));
            }
            if (!out.words.isEmpty()) {
                output.lines.push_back(std::move(out));
            }
        }
        return output;
    }

private:
    BackendStatus status_;
    windows_ocr::Engine engine_;
    int maximumDimension_{2600};
};

} // namespace

std::unique_ptr<Recognizer> CreateWindowsRecognizer()
{
    return std::make_unique<WindowsOcrRecognizer>();
}
#else
std::unique_ptr<Recognizer> CreateWindowsRecognizer()
{
    return {};
}
#endif

std::unique_ptr<Recognizer> CreateDefaultRecognizer(const ModelSettings& models)
{
    return CreateAutomaticRecognizer(models);
}

} // namespace okuflow::uct
