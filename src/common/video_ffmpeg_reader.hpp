#pragma once

#ifdef _WIN32
#include "okuflow/common/video_archive.hpp"
#include <mfreadwrite.h>
#include <chrono>
#include <functional>
#include <string>

namespace okuflow::detail {
// Private, synchronous sample provider; called only on VideoPlayback's owned
// worker. All file access is through independent cursors on the held backing.
bool FfmpegPlaybackRequested();
HRESULT CreateFfmpegVideoReader(IMFByteStream* bytes, const VideoReadStreamClone& clone,
    std::function<bool()> current, std::chrono::steady_clock::time_point deadline,
    IMFSourceReader** reader, std::string& reason);
// Native readers are a no-op. The floor avoids converting discarded preroll.
void ArmFfmpegVideoReader(IMFSourceReader* reader, std::function<bool()> current,
    std::chrono::steady_clock::time_point deadline, std::int64_t copyFloor = -1);
std::string FfmpegVideoReaderError(IMFSourceReader* reader);
}
#endif
