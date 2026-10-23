#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <cstdint>

namespace okuflow::uct::engine_cache {
inline constexpr std::uint64_t kFileLimit = 256ull * 1024 * 1024;
inline constexpr std::uint64_t kDigestBytes = 32;
inline constexpr std::int64_t kContextLimit = 256ll * 1024 * 1024;
inline bool PayloadFits(std::uint64_t bytes) {
    return bytes > 0 && bytes < kFileLimit - kDigestBytes;
}
inline bool FileFits(std::uint64_t bytes) {
    return bytes > kDigestBytes && bytes < kFileLimit;
}
inline bool ContextFits(std::int64_t bytes) {
    return bytes >= 0 && bytes <= kContextLimit;
}
// QTemporaryFile owns the native handle even after close(). Its rename closes
// that handle as needed, is atomic, and refuses an existing destination.
inline bool WriteNew(const QString& path, const QByteArray& payload, QString& error) {
    if (!PayloadFits(static_cast<std::uint64_t>(payload.size()))) {
        error = "serialized plan exceeds cache bound";
        return false;
    }
    QTemporaryFile temporary(QFileInfo(path).dir().filePath(".uct-plan-XXXXXX"));
    if (!temporary.open()) { error = temporary.errorString(); return false; }
    const auto digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    if (temporary.write(digest) != digest.size() || temporary.write(payload) != payload.size() || !temporary.flush()) {
        error = temporary.errorString();
        return false;
    }
    if (!temporary.rename(path)) { error = temporary.errorString(); return false; }
    temporary.setAutoRemove(false);
    error.clear();
    return true;
}
} // namespace okuflow::uct::engine_cache
