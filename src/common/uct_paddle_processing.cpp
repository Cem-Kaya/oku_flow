#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/common/uct_model_pins.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace okuflow::uct
{
namespace
{
bool Verify(const QString &file, const char *hash)
{
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash digest(QCryptographicHash::Sha256);
    return digest.addData(&input) && digest.result().toHex() == hash;
}
QPointF At(const PaddleQuad &q, double x, double y)
{
    return q.points[0] * ((1 - x) * (1 - y)) + q.points[1] * (x * (1 - y)) + q.points[2] * (x * y) +
           q.points[3] * ((1 - x) * y);
}
float Sample(const QImage &image, float x, float y, int channel)
{
    x = std::clamp(x, 0.f, image.width() - 1.f);
    y = std::clamp(y, 0.f, image.height() - 1.f);
    const int ix = static_cast<int>(x), iy = static_cast<int>(y),
              jx = std::min(ix + 1, image.width() - 1), jy = std::min(iy + 1, image.height() - 1);
    const auto a = image.constScanLine(iy), b = image.constScanLine(jy);
    const float top = a[ix * 4 + channel] * (1 - (x - ix)) + a[jx * 4 + channel] * (x - ix);
    const float bottom = b[ix * 4 + channel] * (1 - (x - ix)) + b[jx * 4 + channel] * (x - ix);
    return top * (1 - (y - iy)) + bottom * (y - iy);
}
} // namespace

QString DefaultPaddleRoot()
{
    const QString configured = qEnvironmentVariable("OKUFLOW_UCT_ROOT");
    if (!configured.isEmpty())
        return QDir(configured).absolutePath();
    const QString sibling = QDir(QCoreApplication::applicationDirPath()).filePath("ocr");
    if (QFile::exists(sibling + "/det/inference.onnx"))
        return sibling;
    return QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/ocr-runtime");
}

PaddleAssets LoadPaddleAssets(const QString &root, ModelTier detector, ModelTier recognizer)
{
    PaddleAssets assets;
    assets.root = root;
    const auto* det=FindPaddleModelPin(true,detector);
    const auto* rec=FindPaddleModelPin(false,recognizer);
    if(!det || !rec) {assets.error="Unsupported PaddleOCR model tier.";return assets;}
    assets.detector = QDir(root).filePath(QString::fromUtf8(det->directory.data(),qsizetype(det->directory.size()))+"/inference.onnx");
    assets.recognizer = QDir(root).filePath(QString::fromUtf8(rec->directory.data(),qsizetype(rec->directory.size()))+"/inference.onnx");
    const QString config=QDir(root).filePath(QString::fromUtf8(rec->directory.data(),qsizetype(rec->directory.size()))+"/inference.yml");
    assets.detectorSha256=QByteArray(det->modelSha256.data(),qsizetype(det->modelSha256.size()));
    assets.recognizerSha256=QByteArray(rec->modelSha256.data(),qsizetype(rec->modelSha256.size()));
    if (!Verify(assets.detector,det->modelSha256.data()) ||
        !Verify(assets.recognizer,rec->modelSha256.data()) ||
        !Verify(config,rec->configSha256.data()))
    {
        assets.error = "Pinned PaddleOCR model/config files are missing or changed.";
        return assets;
    }
    QFile input(config);
    if (!input.open(QIODevice::ReadOnly))
    {
        assets.error = "Cannot read OCR dictionary.";
        return assets;
    }
    bool reading = false;
    assets.dictionary.push_back(QString());
    for (const QByteArray &bytes : input.readAll().split('\n'))
    {
        QString line = QString::fromUtf8(bytes).remove('\r');
        if (line == "  character_dict:")
        {
            reading = true;
            continue;
        }
        if (!reading)
            continue;
        if (!line.startsWith("  - "))
            break;
        QString value = line.mid(4);
        if (value.startsWith('\'') && value.endsWith('\''))
            value = value.mid(1, value.size() - 2).replace("''", "'");
        else if (value.startsWith('"'))
        {
            QJsonParseError error;
            const auto array =
                QJsonDocument::fromJson(("[" + value + "]").toUtf8(), &error).array();
            if (error.error != QJsonParseError::NoError || array.size() != 1 ||
                !array[0].isString())
            {
                assets.error = "Invalid pinned OCR dictionary.";
                return assets;
            }
            value = array[0].toString();
        }
        if (value.isEmpty())
        {
            assets.error = "Empty OCR dictionary entry.";
            return assets;
        }
        assets.dictionary.push_back(value);
    }
    if (assets.dictionary.size() < 100)
        assets.error = "Incomplete OCR dictionary.";
    assets.dictionary.push_back(" ");
    return assets;
}

QSize PaddleDetectionSize(QSize source)
{
    if (source.isEmpty())
        return {};
    const double scale = std::min(1.0, 960.0 / std::max(source.width(), source.height()));
    return {std::max(32, static_cast<int>(std::round(source.width() * scale / 32)) * 32),
            std::max(32, static_cast<int>(std::round(source.height() * scale / 32)) * 32)};
}

std::vector<float> PaddleNormalize(const QImage &image, QSize target, bool detection)
{
    if (image.isNull() || target.isEmpty() || target.width() > 4096 || target.height() > 4096)
        throw std::runtime_error("Invalid OCR image dimensions.");
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    const std::size_t plane = static_cast<std::size_t>(target.width()) * target.height();
    std::vector<float> result(plane * 3);
    constexpr float mean[]{.485f, .456f, .406f}, deviation[]{.229f, .224f, .225f};
    for (int y = 0; y < target.height(); ++y)
    {
        for (int x = 0; x < target.width(); ++x)
            for (int channel = 0; channel < 3; ++channel)
            {
                const float sx = (x + .5f) * source.width() / target.width() - .5f;
                const float sy = (y + .5f) * source.height() / target.height() - .5f;
                const float v = Sample(source, sx, sy, channel) / 255.0f; // vendor BGR, not RGB
                result[channel * plane + static_cast<std::size_t>(y) * target.width() + x] =
                    detection ? (v - mean[channel]) / deviation[channel] : (v - .5f) * 2;
            }
    }
    return result;
}

QVector<PaddleQuad> PaddleDetect(std::span<const float> map, int width, int height, QSize source,
                                 const std::atomic<bool> &cancelled)
{
    QVector<PaddleQuad> result;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || source.isEmpty() ||
        map.size() != static_cast<std::size_t>(width) * height)
        return result;
    std::vector<std::uint8_t> visited(map.size());
    std::vector<int> component;
    for (int start = 0; start < static_cast<int>(map.size()); ++start)
    {
        if (cancelled.load())
            return {};
        if (visited[start] || !std::isfinite(map[start]) || map[start] <= .2f)
            continue;
        visited[start] = 1;
        component.clear();
        component.push_back(start);
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        for (std::size_t i = 0; i < component.size(); ++i)
        {
            const int p = component[i], x = p % width, y = p / width;
            sx += x;
            sy += y;
            sxx += x * x;
            syy += y * y;
            sxy += x * y;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= width || ny < 0 || ny >= height)
                        continue;
                    const int n = ny * width + nx;
                    if (!visited[n] && std::isfinite(map[n]) && map[n] > .2f)
                    {
                        visited[n] = 1;
                        component.push_back(n);
                    }
                }
        }
        if (component.size() < 9)
            continue;
        const double count = static_cast<double>(component.size());
        const double angle = .5 * std::atan2(2 * (sxy - sx * sy / count),
                                             sxx - sx * sx / count - syy + sy * sy / count);
        const double c = std::cos(angle), s = std::sin(angle);
        double u0 = 1e10, v0 = 1e10, u1 = -1e10, v1 = -1e10;
        for (int p : component)
        {
            const double u = (p % width) * c + (p / width) * s,
                         v = -(p % width) * s + (p / width) * c;
            u0 = std::min(u0, u);
            u1 = std::max(u1, u);
            v0 = std::min(v0, v);
            v1 = std::max(v1, v);
        }
        double w = u1 - u0 + 1, h = v1 - v0 + 1;
        if (w < 3 || h < 3)
            continue;
        double score = 0;
        int samples = 0;
        const int x0 =
            std::max(0, static_cast<int>(std::floor(std::min({u0 * c - v0 * s, u1 * c - v0 * s,
                                                              u1 * c - v1 * s, u0 * c - v1 * s}))));
        const int x1 = std::min(
            width - 1, static_cast<int>(std::ceil(std::max(
                           {u0 * c - v0 * s, u1 * c - v0 * s, u1 * c - v1 * s, u0 * c - v1 * s}))));
        const int y0 =
            std::max(0, static_cast<int>(std::floor(std::min({u0 * s + v0 * c, u1 * s + v0 * c,
                                                              u1 * s + v1 * c, u0 * s + v1 * c}))));
        const int y1 = std::min(
            height - 1, static_cast<int>(std::ceil(std::max({u0 * s + v0 * c, u1 * s + v0 * c,
                                                             u1 * s + v1 * c, u0 * s + v1 * c}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const double u = x * c + y * s, v = -x * s + y * c;
                if (u >= u0 && u <= u1 && v >= v0 && v <= v1)
                {
                    score += map[static_cast<std::size_t>(y) * width + x];
                    ++samples;
                }
            }
        if (!samples || !std::isfinite(score) || score / samples < .4)
            continue;
        const double expand = w * h * 1.4 / (2 * (w + h));
        u0 -= expand;
        u1 += expand;
        v0 -= expand;
        v1 += expand;
        const auto point = [&](double u, double v) {
            return QPointF(std::clamp((u * c - v * s) * source.width() / width, 0.0,
                                      static_cast<double>(source.width())),
                           std::clamp((u * s + v * c) * source.height() / height, 0.0,
                                      static_cast<double>(source.height())));
        };
        PaddleQuad q{{point(u0, v0), point(u1, v0), point(u1, v1), point(u0, v1)},
                     static_cast<float>(score / samples)};
        if (h > w * 1.5)
            std::rotate(q.points.begin(), q.points.begin() + 3, q.points.end());
        result.push_back(q);
        if (result.size() >= 128)
            break;
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        const double ay = a.points[0].y(), by = b.points[0].y();
        return ay == by ? a.points[0].x() < b.points[0].x() : ay < by;
    });
    return result;
}

std::vector<float> PaddleRectify(const QImage &image, const PaddleQuad &quad, int width,
                                 int validWidth)
{
    if (image.isNull() || width < 32 || width > 2048 || validWidth < 1)
        return {};
    const auto source = image.convertToFormat(QImage::Format_ARGB32);
    std::vector<float> crop(static_cast<std::size_t>(width) * 48 * 3, 0.f);
    const int valid = std::min(width, validWidth);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < valid; ++x)
        {
            const auto p = At(quad, (x + .5f) / valid, (y + .5f) / 48);
            for (int ch = 0; ch < 3; ++ch)
            {
                crop[ch * width * 48 + y * width + x] =
                    (Sample(source, static_cast<float>(p.x()) - .5f,
                            static_cast<float>(p.y()) - .5f, ch) /
                         255 -
                     .5f) *
                    2;
            }
        }
    return crop;
}

RecognizedLine PaddleDecode(std::span<const float> probabilities, int steps, int classes,
                            const PaddleQuad &quad, const QStringList &dictionary)
{
    if (steps <= 0 || classes != dictionary.size() ||
        probabilities.size() != static_cast<std::size_t>(steps) * classes)
        return {};
    std::vector<PaddleToken> tokens;
    int previous = -1;
    for (int t = 0; t < steps; ++t)
    {
        const auto row = probabilities.subspan(static_cast<std::size_t>(t) * classes, classes);
        if (std::any_of(row.begin(), row.end(),
                        [](float v) { return !std::isfinite(v) || v < 0 || v > 1.0001f; }))
            return {};
        const auto best = std::max_element(row.begin(), row.end());
        const int id = static_cast<int>(best - row.begin());
        if (*best < 0 || *best > 1.0001f)
            return {};
        if (id != 0)
        {
            if (id == previous && !tokens.empty())
                tokens.back().last = t;
            else
                tokens.push_back({id, t, t, *best});
        }
        previous = id;
    }
    return PaddleDecodeTokens(tokens, steps, quad, dictionary);
}

RecognizedLine PaddleDecodeTokens(std::span<const PaddleToken> tokens, int steps,
                                  const PaddleQuad &quad, const QStringList &dictionary) {
    RecognizedLine line;
    if (steps <= 0)
        return line;
    QString word;
    int first = 0, last = 0;
    double confidence = 0;
    int count = 0;
    auto flush = [&] {
        if (!word.isEmpty() && count) {
            const double x0 = std::max(0.0, (first - .5) / steps),
                         x1 = std::min(1.0, (last + 1.5) / steps);
            const auto a = At(quad, x0, 0), b = At(quad, x1, 0), c = At(quad, x1, 1),
                       d = At(quad, x0, 1);
            const double left = std::min({a.x(), b.x(), c.x(), d.x()}),
                         top = std::min({a.y(), b.y(), c.y(), d.y()});
            const double right = std::max({a.x(), b.x(), c.x(), d.x()}),
                         bottom = std::max({a.y(), b.y(), c.y(), d.y()});
            const double searchX0 = std::max(0.0, x0 - 1.0 / steps);
            const double searchX1 = std::min(1.0, x1 + 1.0 / steps);
            const auto sa = At(quad, searchX0, 0), sb = At(quad, searchX1, 0),
                       sc = At(quad, searchX1, 1), sd = At(quad, searchX0, 1);
            const double searchLeft = std::min({sa.x(), sb.x(), sc.x(), sd.x()}),
                         searchRight = std::max({sa.x(), sb.x(), sc.x(), sd.x()});
            const QRectF bounds(left, top, right - left, bottom - top);
            const double meanConfidence = confidence / count;
            line.words.push_back({word, bounds, true,
                                  QRectF(searchLeft, top, searchRight - searchLeft, bottom - top),
                                  meanConfidence >= .7 ? TokenDisposition::kCandidate
                                                       : TokenDisposition::kLowConfidence,
                                  meanConfidence, bounds});
        }
        word.clear();
        confidence = 0;
        count = 0;
    };
    for (const auto &token : tokens) {
        if (token.id <= 0 || token.id >= dictionary.size() || token.first < 0 ||
            token.last < token.first || token.last >= steps || !std::isfinite(token.confidence) ||
            token.confidence < 0 || token.confidence > 1.0001f)
            return {};
        if (dictionary[token.id] == " ")
            flush();
        else {
            if (word.isEmpty())
                first = token.first;
            last = token.last;
            word += dictionary[token.id];
            confidence += token.confidence;
            ++count;
        }
    }
    flush();
    for (int i = 0; i < line.words.size(); ++i) {
        auto &current = line.words[i];
        const double left =
            i > 0 ? (line.words[i - 1].boundsPx.right() + current.boundsPx.left()) * .5
                  : current.inkSearchPx.left();
        const double right =
            i + 1 < line.words.size()
                ? (current.boundsPx.right() + line.words[i + 1].boundsPx.left()) * .5
                : current.inkSearchPx.right();
        current.inkSearchPx = current.inkSearchPx.intersected(
            QRectF(left, current.boundsPx.top(), right - left, current.boundsPx.height()));
    }
    return line;
}

} // namespace okuflow::uct
