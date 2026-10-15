#pragma once
#include <QBuffer>
#include <QImage>
// Original synthetic fixture generated in memory with the existing Qt codec.
// No camera/provider/third-party footage; never saved in a release inventory.
inline QByteArray McpFixtureJpeg() {
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(qRgb(128, 128, 128));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, "JPEG", 80) ? bytes : QByteArray();
}
