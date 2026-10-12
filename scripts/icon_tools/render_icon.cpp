// First-party asset exporter. No desktop window or user settings are opened.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QBuffer>
#include <QDataStream>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <vector>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) return 2;
    QSvgRenderer renderer(QString::fromLocal8Bit(argv[1]));
    if (!renderer.isValid()) return 3;
    QDir output(QString::fromLocal8Bit(argv[2]));
    if (!output.mkpath(".")) return 4;
    auto render = [&](int size) {
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.render(&painter);
        painter.end();
        return image;
    };
    if (!render(512).save(output.filePath("okuflow-icon.png"))) return 5;
    for (const auto& asset : std::vector<std::pair<const char*, int>>{
             {"StoreLogo.png", 50}, {"Square44x44Logo.png", 44},
             {"Square150x150Logo.png", 150}}) {
        if (!render(asset.second).save(output.filePath(asset.first))) return 6;
    }
    const std::vector<int> sizes{16, 24, 32, 48, 64, 128, 256};
    std::vector<QByteArray> frames;
    for (int size : sizes) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::WriteOnly) || !render(size).save(&buffer, "PNG")) return 7;
        frames.push_back(bytes);
    }
    QFile file(output.filePath("okuflow.ico"));
    if (!file.open(QIODevice::WriteOnly)) return 8;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0) << quint16(1) << quint16(sizes.size());
    quint32 offset = 6 + 16 * quint32(sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
        const quint8 edge = sizes[i] == 256 ? 0 : quint8(sizes[i]);
        stream << edge << edge << quint8(0) << quint8(0) << quint16(1) << quint16(32)
               << quint32(frames[i].size()) << offset;
        offset += quint32(frames[i].size());
    }
    for (const auto& bytes : frames) {
        if (stream.writeRawData(bytes.constData(), int(bytes.size())) != bytes.size()) return 9;
    }
    return stream.status() == QDataStream::Ok && file.flush() ? 0 : 10;
}
