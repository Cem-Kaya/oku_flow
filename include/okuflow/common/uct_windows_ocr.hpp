#pragma once

// Qt-free wrapper around the built-in Windows.Media.Ocr engine. Kept apart
// from Qt headers so C++/WinRT compiles in its own translation unit.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace okuflow::uct::windows_ocr {

struct Word {
    std::wstring text;
    float x{};
    float y{};
    float width{};
    float height{};
};

struct Line {
    std::vector<Word> words;
};

struct Result {
    bool ok{};
    bool cancelled{};
    std::string error;
    std::vector<Line> lines;
};

struct InitResult {
    bool ready{};
    std::wstring languageTag;
    std::string detail;
    unsigned int maxImageDimension{};
};

// Use from a single worker thread. Initialize joins that thread to the
// multithreaded COM apartment; OCR never runs on the Qt UI thread.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    InitResult Initialize();
    // 32-bit BGRA rows. Cancellation requests IAsyncOperation::Cancel and
    // still waits (bounded) for the operation to settle before returning.
    Result Recognize(const std::uint8_t* bgra, int width, int height, int strideBytes,
                     const std::atomic<bool>& cancelled);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace okuflow::uct::windows_ocr
