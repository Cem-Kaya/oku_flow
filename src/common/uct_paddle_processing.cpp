#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/common/uct_model_pins.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineF>
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

QVector<QRect> PaddleDetectionRegions(QSize source)
{
    if (source.isEmpty()) return {};
    const QRect whole(QPoint{},source);
    QVector<QRect> regions{whole};
    // Explicit CPU images historically supported larger extents. Keep their
    // bounded whole-scene pass rather than report successful empty OCR.
    if(source.width()>8192 || source.height()>8192) return regions;
    constexpr int overlap=160, desiredEdge=1280;
    const auto count=[](int extent) {
        return std::clamp((extent-overlap+desiredEdge-overlap-1)/(desiredEdge-overlap),1,4);
    };
    const int columns=count(source.width()), rows=count(source.height());
    if(columns==1 && rows==1) return regions;
    const int width=(source.width()+(columns-1)*overlap+columns-1)/columns;
    const int height=(source.height()+(rows-1)*overlap+rows-1)/rows;
    for(int y=0;y<rows;++y) for(int x=0;x<columns;++x) {
        const int left=std::min(x*(width-overlap),source.width()-width);
        const int top=std::min(y*(height-overlap),source.height()-height);
        regions.push_back(QRect(left,top,width,height));
    }
    return regions;
}

QVector<PaddleQuad> PaddleRecognitionWindows(const PaddleQuad& line)
{
    QVector<PaddleQuad> result{line};
    const double width=QLineF(line.points[0],line.points[1]).length();
    const double height=QLineF(line.points[0],line.points[3]).length();
    if(!std::isfinite(width) || !std::isfinite(height) || height<1 || width/height<=20) return result;
    const int count=std::clamp(int(std::ceil((width/height-8)/12)),2,8);
    // At the count cap, widen windows instead of leaving gaps or losing
    // the context needed to discard cut-edge fragments safely.
    const double span=std::max(20.0,(width/height+8*(count-1))/count);
    const double fraction=std::min(1.0,span*height/width);
    for(int i=0;i<count;++i) {
        const double start=i*(1-fraction)/(count-1), end=start+fraction;
        auto q=line;
        q.points={At(line,start,0),At(line,end,0),At(line,end,1),At(line,start,1)};
        q.internalEdges=(i>0?1:0)|(i+1<count?2:0);
        result.push_back(q);
    }
    return result;
}

void PaddleMergeRecognition(QVector<RecognizedLine>& output, RecognizedLine line,
                             QRect region, QSize source)
{
    const bool detail = region.size() != source;
    const auto area = [](const QRectF& r) {
        return std::max(0.0, r.width()) * std::max(0.0, r.height());
    };
    const auto overlaps = [&](const RecognizedWord& a, const RecognizedWord& b) {
        const double smaller = std::min(area(a.boundsPx), area(b.boundsPx));
        if (smaller <= 0) return false;
        const double intersection = area(a.boundsPx.intersected(b.boundsPx));
        if (intersection >= .7 * smaller) return true;
        if (intersection < .65 * smaller) return false;
        const QString& shortText = a.text.size() < b.text.size() ? a.text : b.text;
        const QString& longText = a.text.size() < b.text.size() ? b.text : a.text;
        // A crop can leave a prefix/suffix letter just outside the complete
        // token's CTC estimate. Do not let that duplicate protect the very
        // letter the complete, stronger word would replace.
        const bool fragment = !shortText.isEmpty() && shortText.size() < longText.size() &&
            smaller < .35 * std::max(area(a.boundsPx), area(b.boundsPx)) &&
            (longText.startsWith(shortText) || longText.endsWith(shortText));
        return fragment;
    };
    const auto rowDistance = [](const RecognizedLine& row, const RecognizedWord& word) {
        double best = std::numeric_limits<double>::max();
        for (const auto& old : row.words) {
            const double small = std::min(old.boundsPx.height(), word.boundsPx.height());
            const double large = std::max(old.boundsPx.height(), word.boundsPx.height());
            const double dy = std::abs(old.boundsPx.center().y() - word.boundsPx.center().y());
            const double gap = std::max({0.0, old.boundsPx.left() - word.boundsPx.right(),
                                        word.boundsPx.left() - old.boundsPx.right()});
            // Match a nearby word of comparable type size. A row's union
            // must not bridge another column or pull a title into code text.
            if (small > 0 && small >= .5 * large && dy <= .35 * large && gap <= 4 * small)
                best = std::min(best, gap + 4 * dy);
        }
        return best;
    };
    qsizetype preferredLine = -1;
    for (auto word : line.words) {
        // Overlapping crops supply complete words instead of cut-edge fragments.
        constexpr double margin = 8.0;
        if (detail && ((region.left() > 0 && word.boundsPx.left() < margin) ||
            (region.top() > 0 && word.boundsPx.top() < margin) ||
            (region.x() + region.width() < source.width() && word.boundsPx.right() > region.width() - margin) ||
            (region.y() + region.height() < source.height() && word.boundsPx.bottom() > region.height() - margin))) continue;
        word.boundsPx.translate(region.topLeft());
        if (!word.inkSearchPx.isEmpty()) word.inkSearchPx.translate(region.topLeft());
        if (!word.protectedBoundsPx.isEmpty()) word.protectedBoundsPx.translate(region.topLeft());
        bool use = true;
        qsizetype target = -1;
        for (qsizetype index = 0; index < output.size(); ++index) {
            for (const auto& old : output[index].words) {
                if (!overlaps(word, old)) continue;
                if (target < 0) target = index;
                if (word.confidence < old.confidence ||
                    (word.disposition != TokenDisposition::kCandidate && old.disposition == TokenDisposition::kCandidate)) {
                    use = false;
                    break;
                }
            }
            if (!use) break;
        }
        if (target >= 0) preferredLine = target;
        if (!use) continue;
        if (target < 0 && preferredLine >= 0 &&
            rowDistance(output[preferredLine], word) < std::numeric_limits<double>::max())
            target = preferredLine;
        qsizetype emptyLine = -1;
        if (target < 0) {
            double best = std::numeric_limits<double>::max();
            for (qsizetype index = 0; index < output.size(); ++index) {
                const auto& words = output[index].words;
                if (words.isEmpty()) { emptyLine = index; continue; }
                const double distance = rowDistance(output[index], word);
                if (distance < best) {
                    best = distance;
                    target = index;
                }
            }
        }
        if (target < 0) {
            if (emptyLine >= 0) target = emptyLine;
            else {
                // Refuse before erasing any predecessor. A matching word at
                // the cap is still replaced inside its existing logical line.
                if (output.size() >= 1024) continue;
                target = output.size();
                output.push_back({});
            }
        }
        for (auto& oldLine : output) {
            oldLine.words.erase(std::remove_if(oldLine.words.begin(), oldLine.words.end(),
                [&](const auto& old) { return overlaps(word, old); }), oldLine.words.end());
        }
        output[target].words.push_back(std::move(word));
        preferredLine = target;
    }
    output.erase(std::remove_if(output.begin(), output.end(), [](const auto& l) {
        return l.words.isEmpty();
    }), output.end());
    for (auto& row : output) std::stable_sort(row.words.begin(), row.words.end(), [](const auto& a, const auto& b) {
        return a.boundsPx.left() < b.boundsPx.left();
    });
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
            const double context=2*QLineF(quad.points[0],quad.points[3]).length()/
                std::max(1.0,QLineF(quad.points[0],quad.points[1]).length());
            if (!((quad.internalEdges&1 && x0<context) || (quad.internalEdges&2 && x1>1-context)))
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
