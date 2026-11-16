#pragma once
#include <QByteArray>
#include <QCryptographicHash>
#include <QIODevice>
#include <functional>
#include <algorithm>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#ifdef _MSC_VER
// Every target that includes this header needs CNG, also the test targets
// that compile the OCR sources without the shared OCR link settings.
#pragma comment(lib, "bcrypt")
#endif
#endif

namespace okuflow::uct::sha256 {
// Same SHA-256 wire digest as Qt. Per-call CNG state; no process verification
// cache. A failed/cancelled read never returns a partial digest: every
// rejected operation latches the object invalid, an invalid object yields an
// empty digest, and Finish works once. Callers must treat an empty digest as
// "not verified", never as a value to compare.
class Digest {
public:
    Digest() {
#ifdef _WIN32
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            algorithm_ = nullptr;
            return;
        }
        ULONG size{}, received{};
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size),
                              sizeof(size), &received, 0) < 0 || received != sizeof(size) ||
            !size || size > 1024 * 1024) return;
        try { object_.resize(size); }
        catch (...) { BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr; throw; }
        valid_ = BCryptCreateHash(algorithm_, &hash_, object_.data(), size, nullptr, 0, 0) >= 0;
        if (!valid_) hash_ = nullptr;
#else
        valid_ = true;
#endif
    }
    ~Digest() {
#ifdef _WIN32
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
#endif
    }
    Digest(const Digest&) = delete;
    Digest& operator=(const Digest&) = delete;
    bool Add(const char* data, qsizetype size) {
        if (!valid_ || size < 0 || size > 1024 * 1024 || (size && !data)) return valid_ = false;
#ifdef _WIN32
        valid_ = BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), ULONG(size), 0) >= 0;
#else
        hash_.addData(QByteArrayView(data, size));
#endif
        return valid_;
    }
    QByteArray Finish() {
        if (!valid_) return {};
        valid_ = false; // one result per object, also when finishing fails
#ifdef _WIN32
        QByteArray result(32, '\0');
        if (BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(result.data()), 32, 0) < 0) return {};
        return result;
#else
        return hash_.result();
#endif
    }
private:
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
    std::vector<unsigned char> object_;
#else
    QCryptographicHash hash_{QCryptographicHash::Sha256};
#endif
    bool valid_{};
};
inline QByteArray Bytes(QByteArrayView bytes, const std::function<bool()>& stop = {}) {
    Digest digest;
    for (qsizetype at = 0; at < bytes.size();) {
        if (stop && stop()) return {};
        const auto count = std::min<qsizetype>(1024 * 1024, bytes.size() - at);
        if (!digest.Add(bytes.data() + at, count)) return {};
        at += count;
    }
    if (stop && stop()) return {};
    return digest.Finish();
}
inline QByteArray Read(QIODevice& input, const std::function<bool()>& stop = {}) {
    if (!input.isOpen() || !input.isReadable()) return {};
    Digest digest;
    QByteArray buffer(1024 * 1024, '\0');
    for (;;) {
        if (stop && stop()) return {};
        const auto count = input.read(buffer.data(), buffer.size());
        if (count < 0) return {};
        if (count == 0) {
            if (!input.atEnd()) return {}; // refuse a stalled/nonblocking source
            break;
        }
        if (!digest.Add(buffer.constData(), count)) return {};
    }
    if (stop && stop()) return {};
    return digest.Finish();
}
} // namespace okuflow::uct::sha256
