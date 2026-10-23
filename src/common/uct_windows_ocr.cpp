#include "okuflow/common/uct_windows_ocr.hpp"

#if defined(_WIN32)

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace okuflow::uct::windows_ocr {
namespace {

namespace wf = winrt::Windows::Foundation;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wmo = winrt::Windows::Media::Ocr;
namespace wss = winrt::Windows::Storage::Streams;

std::string HresultText(const char* what, winrt::hresult code)
{
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "%s (0x%08X)", what,
                  static_cast<unsigned int>(code.value));
    return buffer;
}

// Completion state shared with the WinRT callback, so a callback arriving
// after a bounded cancellation wait never touches a dead stack frame.
struct CompletionState {
    winrt::handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<int> status{static_cast<int>(wf::AsyncStatus::Started)};
};

} // namespace

struct Engine::Impl {
    wmo::OcrEngine engine{nullptr};
    bool apartmentJoined{};
};

Engine::Engine()
    : impl_(std::make_unique<Impl>())
{
}

Engine::~Engine() = default;

InitResult Engine::Initialize()
{
    InitResult result;
    try {
        if (!impl_->apartmentJoined) {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (const winrt::hresult_error& error) {
                // Another component already chose this thread's apartment.
                if (error.code() != RPC_E_CHANGED_MODE) {
                    throw;
                }
            }
            impl_->apartmentJoined = true;
        }
        wmo::OcrEngine engine = wmo::OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            const auto languages = wmo::OcrEngine::AvailableRecognizerLanguages();
            if (languages && languages.Size() > 0) {
                engine = wmo::OcrEngine::TryCreateFromLanguage(languages.GetAt(0));
            }
        }
        if (!engine) {
            result.detail = "Windows has no OCR-capable language installed.";
            return result;
        }
        impl_->engine = engine;
        result.ready = true;
        result.languageTag = std::wstring(engine.RecognizerLanguage().LanguageTag());
        result.maxImageDimension = wmo::OcrEngine::MaxImageDimension();
        result.detail = "Windows.Media.Ocr ready.";
    } catch (const winrt::hresult_error& error) {
        result.ready = false;
        result.detail = HresultText("Windows.Media.Ocr could not start", error.code());
    } catch (...) {
        result.ready = false;
        result.detail = "Windows.Media.Ocr could not start.";
    }
    return result;
}

Result Engine::Recognize(const std::uint8_t* bgra, int width, int height, int strideBytes,
                         const std::atomic<bool>& cancelled)
{
    Result result;
    if (!impl_->engine) {
        result.error = "OCR engine is not initialized.";
        return result;
    }
    if (!bgra || width <= 0 || height <= 0 || strideBytes < width * 4) {
        result.error = "Invalid OCR image.";
        return result;
    }
    try {
        const std::uint32_t rowBytes = static_cast<std::uint32_t>(width) * 4;
        const std::uint32_t total = rowBytes * static_cast<std::uint32_t>(height);
        wss::Buffer buffer(total);
        std::uint8_t* destination = buffer.data();
        for (int y = 0; y < height; ++y) {
            std::memcpy(destination + static_cast<std::size_t>(y) * rowBytes,
                        bgra + static_cast<std::size_t>(y) * strideBytes, rowBytes);
        }
        buffer.Length(total);
        const wgi::SoftwareBitmap bitmap = wgi::SoftwareBitmap::CreateCopyFromBuffer(
            buffer, wgi::BitmapPixelFormat::Bgra8, width, height, wgi::BitmapAlphaMode::Ignore);

        auto operation = impl_->engine.RecognizeAsync(bitmap);
        auto state = std::make_shared<CompletionState>();
        if (!state->event) {
            result.error = "Could not create OCR completion event.";
            operation.Cancel();
            return result;
        }
        operation.Completed([state](const auto&, wf::AsyncStatus status) {
            state->status.store(static_cast<int>(status));
            SetEvent(state->event.get());
        });
        bool cancelRequested = false;
        DWORD cancelWaitMs = 0;
        for (;;) {
            if (WaitForSingleObject(state->event.get(), 50) == WAIT_OBJECT_0) {
                break;
            }
            if (!cancelRequested && cancelled.load()) {
                cancelRequested = true;
                operation.Cancel();
            }
            if (cancelRequested) {
                cancelWaitMs += 50;
                if (cancelWaitMs >= 5000) {
                    // The operation keeps its own bitmap reference; returning
                    // abandons only the result, not memory still in use.
                    result.cancelled = true;
                    result.error = "OCR cancellation did not settle within 5 s.";
                    return result;
                }
            }
        }
        const auto status = static_cast<wf::AsyncStatus>(state->status.load());
        if (status == wf::AsyncStatus::Canceled) {
            result.cancelled = true;
            return result;
        }
        if (status != wf::AsyncStatus::Completed) {
            result.error = HresultText("Windows.Media.Ocr failed", operation.ErrorCode());
            return result;
        }
        const wmo::OcrResult ocr = operation.GetResults();
        if (const auto angle = ocr.TextAngle()) result.textAngleDegrees = angle.Value();
        for (const wmo::OcrLine& line : ocr.Lines()) {
            Line outLine;
            for (const wmo::OcrWord& word : line.Words()) {
                const wf::Rect r = word.BoundingRect();
                outLine.words.push_back({std::wstring(word.Text()), r.X, r.Y, r.Width, r.Height});
            }
            if (!outLine.words.empty()) {
                result.lines.push_back(std::move(outLine));
            }
        }
        result.ok = !cancelled.load();
        result.cancelled = cancelled.load();
    } catch (const winrt::hresult_error& error) {
        result.error = HresultText("Windows.Media.Ocr failed", error.code());
    } catch (...) {
        result.error = "Windows.Media.Ocr failed.";
    }
    return result;
}

} // namespace okuflow::uct::windows_ocr

#endif // _WIN32
