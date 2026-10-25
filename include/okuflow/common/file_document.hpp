#pragma once
#ifdef _WIN32
#include <QImage>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

namespace okuflow {
struct FileDocumentSnapshot {
    enum class State { Closed, Opening, Ready, Failed };
    State state{State::Closed};
    std::uint64_t generation{}, documentGeneration{};
    QString path, text, error;
    QImage image;
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;
    int page{}, pageCount{}, longEdge{};
    bool pdf{};
};

// One serial, latest-request-only worker owns the held file and PDF parser.
// Close/destruction revoke publication without joining a third-party decoder.
class FileDocument final {
public:
    FileDocument();
    ~FileDocument();
    FileDocument(const FileDocument&) = delete;
    FileDocument& operator=(const FileDocument&) = delete;
    std::uint64_t Open(QString path, int longEdge = 4096);
    std::uint64_t SelectPage(int page, int longEdge = 4096);
    void Close();
    FileDocumentSnapshot Snapshot() const;
    FileDocumentSnapshot TakeSnapshot();
    static bool SupportsPath(const QString& path);
private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};
}
#endif
