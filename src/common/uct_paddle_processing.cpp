#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/common/uct_model_pins.hpp"
#include "uct_runtime_paths.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineF>
#include <QStandardPaths>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

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
    if (q.ribbon.valid) {
        const auto p = q.ribbon.At(x, y);
        return {p.x, p.y};
    }
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
    return runtime_paths::PaddleRoot(qEnvironmentVariable("OKUFLOW_UCT_ROOT"),
        QCoreApplication::applicationDirPath(),qEnvironmentVariable("LOCALAPPDATA"));
}

PaddleAssets LoadPaddleAssets(const QString &root, ModelTier detector, ModelTier recognizer)
{
    PaddleAssets assets;
    assets.root = root;
    const auto* det=FindPaddleModelPin(true,detector);
    const auto* rec=FindPaddleModelPin(false,recognizer);
    if(!det || !rec) {assets.error="Unsupported PaddleOCR model tier.";return assets;}
    const auto directory = [&](const PaddleModelPin& pin) {
        const auto original = QDir(root).filePath(QString::fromUtf8(pin.directory.data(),qsizetype(pin.directory.size())));
        // Explicit diagnostic roots remain isolated. The production source root
        // may use an optional downloaded role without relocating runtime DLLs.
        if (QDir::cleanPath(root).compare(QDir::cleanPath(DefaultPaddleRoot()),Qt::CaseInsensitive) != 0 ||
            (Verify(QDir(original).filePath("inference.onnx"),pin.modelSha256.data()) &&
             Verify(QDir(original).filePath("inference.yml"),pin.configSha256.data()))) return original;
        const auto revision=QString::fromUtf8(pin.revision.data(),qsizetype(pin.revision.size()));
        return QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/model-cache/sources/ocr-"+
            QString(pin.detection ? "det-" : "rec-")+ModelTierToken(pin.tier)+"-"+revision);
    };
    const auto detectorDirectory=directory(*det), recognizerDirectory=directory(*rec);
    assets.detector = QDir(detectorDirectory).filePath("inference.onnx");
    assets.recognizer = QDir(recognizerDirectory).filePath("inference.onnx");
    const QString config=QDir(recognizerDirectory).filePath("inference.yml");
    assets.detectorSha256=QByteArray(det->modelSha256.data(),qsizetype(det->modelSha256.size()));
    assets.recognizerSha256=QByteArray(rec->modelSha256.data(),qsizetype(rec->modelSha256.size()));
    if (!Verify(assets.detector,det->modelSha256.data()) ||
        !Verify(assets.recognizer,rec->modelSha256.data()))
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
    // Verify and parse the SAME bounded snapshot. Reopening a previously
    // verified file could give token IDs a different meaning during replacement.
    constexpr qint64 kMaximumConfigBytes = 8 * 1024 * 1024;
    const auto configBytes = input.read(kMaximumConfigBytes + 1);
    if (input.error() != QFileDevice::NoError || !input.atEnd() || configBytes.size() > kMaximumConfigBytes ||
        QCryptographicHash::hash(configBytes,QCryptographicHash::Sha256).toHex() != rec->configSha256.data())
    {
        assets.error = "Pinned PaddleOCR model/config files are missing or changed.";
        return assets;
    }
    bool reading = false;
    assets.dictionary.push_back(QString());
    for (const QByteArray &bytes : configBytes.split('\n'))
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

QSize PaddleDetectionSize(QSize source, double maximumMagnification, int maximumLongEdge)
{
    return DetectorInputSize(source, maximumMagnification, maximumLongEdge);
}

QVector<QRect> PaddleDetectionRegions(QSize source)
{
    return DetectorRegions(source);
}

double PaddleDetailMagnification(const DetailFocus& focus)
{
    return DetailMagnification(focus);
}

QRect PaddleDetailRegion(QSize source, const DetailFocus& focus)
{
    return DetailRegion(source, focus);
}

QVector<QRect> PaddleRecognitionRegions(QSize source, const std::optional<DetailFocus>& focus)
{
    if (!focus) return DetectorRegions(source);
    const QRect region = DetailRegion(source, *focus);
    return region.isEmpty() ? QVector<QRect>{} : QVector<QRect>{region};
}

QVector<PaddleQuad> PaddleRecognitionWindows(const PaddleQuad& line)
{
    QVector<PaddleQuad> result{line};
    const double width=PaddleRecognitionWidth(line);
    const double height=PaddleRecognitionHeight(line);
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
        if (line.ribbon.valid) {
            q.ribbon.start=line.ribbon.start+(line.ribbon.end-line.ribbon.start)*start;
            q.ribbon.end=line.ribbon.start+(line.ribbon.end-line.ribbon.start)*end;
        }
        q.internalEdges=(i>0?1:0)|(i+1<count?2:0);
        result.push_back(q);
    }
    return result;
}

double PaddleRecognitionWidth(const PaddleQuad& line) {
    return line.ribbon.valid ? line.ribbon.Width() : QLineF(line.points[0],line.points[1]).length();
}
double PaddleRecognitionHeight(const PaddleQuad& line) {
    return line.ribbon.valid ? line.ribbon.Height() : QLineF(line.points[0],line.points[3]).length();
}

namespace
{
// The per-call merge, unchanged: used above the context's bounds.
void MergeWithoutContext(QVector<RecognizedLine>& output, RecognizedLine line,
                         QRect region, QSize source)
{
    const bool detail = region.size() != source;
    // Rotated words compare their detector shapes. Bounding boxes of
    // neighbouring tilted lines overlap even where the text does not.
    const auto geometry = [](const RecognizedWord& word) {
        return word.quadPx.valid ? word.quadPx : TextQuad::FromRect(word.boundsPx);
    };
    struct WordGeometry {
        TextQuad shape;
        QRectF bounds;
        std::optional<double> area;
        double angle{}, height{};
        QPointF center;
        bool rowReady{}, overlap{};
    };
    const auto makeGeometry = [&](const RecognizedWord& word) {
        WordGeometry result;
        result.shape = geometry(word);
        result.bounds = result.shape.Bounds();
        return result;
    };
    const auto area = [](WordGeometry& value) {
        if (!value.area) value.area = QuadArea(value.shape);
        return *value.area;
    };
    const auto prepareRow = [](WordGeometry& value) {
        if (value.rowReady) return;
        value.angle = QuadAngle(value.shape);
        value.height = QuadHeight(value.shape);
        value.center = value.shape.Center();
        value.rowReady = true;
    };
    // The cache belongs to this call and follows the original line/word
    // indices until the final prune/sort. Oversize caller-supplied output
    // keeps the uncached decisions rather than acquiring an unbounded cache.
    constexpr std::size_t maximumCachedWords = 8192;
    std::size_t cachedWords = 0;
    std::size_t cachedCapacity = 0;
    bool cached = output.size() <= 1024;
    for (const auto& row : output) {
        if (!cached || std::size_t(row.words.size()) > maximumCachedWords - cachedWords) {
            cached = false;
            break;
        }
        cachedWords += std::size_t(row.words.size());
    }
    std::vector<std::vector<WordGeometry>> cache;
    if (cached) {
        cache.resize(std::size_t(output.size()));
        for (qsizetype index = 0; index < output.size(); ++index) {
            auto& row = cache[std::size_t(index)];
            row.reserve(std::size_t(output[index].words.size()));
            cachedCapacity += row.capacity();
            for (const auto& old : output[index].words) row.push_back(makeGeometry(old));
        }
        if (cachedCapacity > maximumCachedWords) { cached = false; cache.clear(); }
    }
    const auto overlaps = [&](const RecognizedWord& a, const RecognizedWord& b,
                              WordGeometry& ga, WordGeometry& gb) {
        // A rejected bounding pair produces zero intersection in the old
        // clipper as well. In particular this is not an epsilon/IoU test.
        if (!ga.bounds.intersects(gb.bounds)) return false;
        const double areaA = area(ga), areaB = area(gb);
        const double smaller = std::min(areaA, areaB);
        if (smaller <= 0) return false;
        const double intersection = QuadIntersectionArea(ga.shape, gb.shape);
        if (intersection >= .7 * smaller) return true;
        if (intersection < .65 * smaller) return false;
        const QString& shortText = a.text.size() < b.text.size() ? a.text : b.text;
        const QString& longText = a.text.size() < b.text.size() ? b.text : a.text;
        // A crop can leave a prefix/suffix letter just outside the complete
        // token's CTC estimate. Do not let that duplicate protect the very
        // letter the complete, stronger word would replace.
        const bool fragment = !shortText.isEmpty() && shortText.size() < longText.size() &&
            smaller < .35 * std::max(areaA, areaB) &&
            (longText.startsWith(shortText) || longText.endsWith(shortText));
        return fragment;
    };
    const auto rowDistance = [&](qsizetype index, WordGeometry& wordGeometry) {
        double best = std::numeric_limits<double>::max();
        prepareRow(wordGeometry);
        const TextQuad& wordShape = wordGeometry.shape;
        const double angle = wordGeometry.angle;
        const QPointF along(std::cos(angle), std::sin(angle)), across(-along.y(), along.x());
        const auto extent = [&](const TextQuad& quad) {
            double low = std::numeric_limits<double>::max(), high = std::numeric_limits<double>::lowest();
            for (const QPointF& point : quad.points) {
                const double t = QPointF::dotProduct(point, along);
                low = std::min(low, t); high = std::max(high, t);
            }
            return std::pair{low, high};
        };
        const auto [wordLow, wordHigh] = extent(wordShape);
        const auto& row = output[index];
        for (qsizetype wi = 0; wi < row.words.size(); ++wi) {
            WordGeometry local;
            auto& oldGeometry = cached ? cache[std::size_t(index)][std::size_t(wi)] :
                (local = makeGeometry(row.words[wi]));
            prepareRow(oldGeometry);
            const TextQuad& oldShape = oldGeometry.shape;
            // Differently tilted text is a different line even when the
            // bounding boxes of the two rotated rows interleave.
            double turn = std::abs(oldGeometry.angle - angle);
            turn = std::fmod(turn, std::numbers::pi);
            if (std::min(turn, std::numbers::pi - turn) > .175) continue;
            const double small = std::min(oldGeometry.height, wordGeometry.height);
            const double large = std::max(oldGeometry.height, wordGeometry.height);
            const double dy = std::abs(QPointF::dotProduct(oldGeometry.center - wordGeometry.center, across));
            const auto [oldLow, oldHigh] = extent(oldShape);
            const double gap = std::max({0.0, oldLow - wordHigh, wordLow - oldHigh});
            // Match a nearby word of comparable type size. A row's union
            // must not bridge another column or pull a title into code text.
            if (small > 0 && small >= .5 * large && dy <= .35 * large && gap <= 4 * small)
                best = std::min(best, gap + 4 * dy);
        }
        return best;
    };
    // Only refused ribbon geometry carries a hard envelope. Ordinary padded
    // rectangle words may also have larger protection: that is not evidence
    // of a curve. A replacement carrying an unresolved ribbon mask remains
    // invalid geometry with unknown error (-1), retaining non-carvable ink
    // through PrepareLines/model admission and further merge replacements.
    const auto inheritableProtection = [](const RecognizedWord& value) {
        return !value.protectedBoundsPx.isEmpty() &&
            value.disposition==TokenDisposition::kInvalidGeometry &&
            (value.fromRibbon || value.ribbonErrorPx<0);
    };
    qsizetype preferredLine = -1;
    for (auto word : line.words) {
        // Overlapping crops supply complete words instead of cut-edge fragments.
        constexpr double margin = kDetailCropEdgeMarginPx;
        if (detail && ((region.left() > 0 && word.boundsPx.left() < margin) ||
            (region.top() > 0 && word.boundsPx.top() < margin) ||
            (region.x() + region.width() < source.width() && word.boundsPx.right() > region.width() - margin) ||
            (region.y() + region.height() < source.height() && word.boundsPx.bottom() > region.height() - margin))) continue;
        word.boundsPx.translate(region.topLeft());
        if (!word.inkSearchPx.isEmpty()) word.inkSearchPx.translate(region.topLeft());
        if (!word.protectedBoundsPx.isEmpty()) word.protectedBoundsPx.translate(region.topLeft());
        if (word.quadPx.valid) word.quadPx = word.quadPx.Translated(region.topLeft());
        if (word.inkSearchQuadPx.valid) word.inkSearchQuadPx = word.inkSearchQuadPx.Translated(region.topLeft());
        if (word.fromRibbon) word.ribbonReadingPointPx += region.topLeft();
        auto wordGeometry = makeGeometry(word);
        bool use = true;
        qsizetype dropLine = -1, dropWord = -1;
        QRectF inheritedProtection;
        qsizetype target = -1;
        for (qsizetype index = 0; index < output.size(); ++index) {
            const auto& words = std::as_const(output)[index].words;
            for (qsizetype wi = 0; wi < words.size(); ++wi) {
                const auto& old = words[wi];
                WordGeometry local;
                auto& oldGeometry = cached ? cache[std::size_t(index)][std::size_t(wi)] :
                    (local = makeGeometry(old));
                oldGeometry.overlap = overlaps(word, old, wordGeometry, oldGeometry);
                if (!oldGeometry.overlap) continue;
                if (target < 0) target = index;
                if (word.confidence < old.confidence ||
                    (word.disposition != TokenDisposition::kCandidate && old.disposition == TokenDisposition::kCandidate)) {
                    use = false;
                    dropLine = index;
                    dropWord = wi;
                    break;
                }
            }
            if (!use) break;
        }
        if (target >= 0) preferredLine = target;
        if (!use) {
            if (inheritableProtection(word) && dropLine >= 0) {
                auto& winner = output[dropLine].words[dropWord];
                winner.protectedBoundsPx = winner.protectedBoundsPx.isEmpty()
                    ? word.protectedBoundsPx
                    : winner.protectedBoundsPx.united(word.protectedBoundsPx);
                winner.disposition=TokenDisposition::kInvalidGeometry;
                winner.ribbonErrorPx=-1;
            }
            continue;
        }
        if (target < 0 && preferredLine >= 0 &&
            rowDistance(preferredLine, wordGeometry) < std::numeric_limits<double>::max())
            target = preferredLine;
        qsizetype emptyLine = -1;
        if (target < 0) {
            double best = std::numeric_limits<double>::max();
            for (qsizetype index = 0; index < output.size(); ++index) {
                const auto& words = output[index].words;
                if (words.isEmpty()) { emptyLine = index; continue; }
                const double distance = rowDistance(index, wordGeometry);
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
                if (cached) cache.emplace_back();
            }
        }
        for (qsizetype index = 0; index < output.size(); ++index) {
            auto& words = output[index].words;
            if (cached) {
                // A usable word completed the entire first scan with no
                // output mutations. Reuse exactly those decisions; empty
                // rows keep their indices until the end of this call.
                auto& row = cache[std::size_t(index)];
                qsizetype kept = 0;
                for (qsizetype wi = 0; wi < words.size(); ++wi) {
                    if (row[std::size_t(wi)].overlap) {
                        if (inheritableProtection(words[wi]))
                            inheritedProtection = inheritedProtection.isEmpty()
                                ? words[wi].protectedBoundsPx
                                : inheritedProtection.united(words[wi].protectedBoundsPx);
                        --cachedWords; continue;
                    }
                    if (kept != wi) {
                        words[kept] = std::move(words[wi]);
                        row[std::size_t(kept)] = std::move(row[std::size_t(wi)]);
                    }
                    ++kept;
                }
                words.erase(words.begin() + kept, words.end());
                row.resize(std::size_t(kept));
            } else {
                words.erase(std::remove_if(words.begin(), words.end(), [&](const auto& old) {
                    auto oldGeometry = makeGeometry(old);
                    const bool hit = overlaps(word, old, wordGeometry, oldGeometry);
                    if (hit && inheritableProtection(old))
                        inheritedProtection = inheritedProtection.isEmpty()
                            ? old.protectedBoundsPx
                            : inheritedProtection.united(old.protectedBoundsPx);
                    return hit;
                }), words.end());
            }
        }
        if (!inheritedProtection.isEmpty()) {
            word.protectedBoundsPx = word.protectedBoundsPx.isEmpty()
                ? inheritedProtection
                : word.protectedBoundsPx.united(inheritedProtection);
            word.disposition=TokenDisposition::kInvalidGeometry;
            word.ribbonErrorPx=-1;
        }
        output[target].words.push_back(std::move(word));
        if (cached) {
            auto& row = cache[std::size_t(target)];
            // Deleted rows may retain spare vector capacity. Account for that
            // too, so repeatedly moving words between rows cannot grow the
            // call-local cache past the limit.
            if (row.size() == row.capacity() && cachedCapacity < maximumCachedWords) {
                const auto oldCapacity = row.capacity();
                row.reserve(row.size() + 1);
                cachedCapacity += row.capacity() - oldCapacity;
            }
            if (cachedWords == maximumCachedWords || cachedCapacity > maximumCachedWords ||
                row.size() == row.capacity()) {
                cached = false;
                cache.clear();
            } else {
                row.push_back(std::move(wordGeometry));
                ++cachedWords;
            }
        }
        preferredLine = target;
    }
    output.erase(std::remove_if(output.begin(), output.end(), [](const auto& l) {
        return l.words.isEmpty();
    }), output.end());
    // Reading order along the row's own baseline (left edge when upright).
    for (auto& row : output) {
        const double angle = QuadAngle(geometry(row.words.first()));
        const QPointF along(std::cos(angle), std::sin(angle));
        std::stable_sort(row.words.begin(), row.words.end(), [&](const auto& a, const auto& b) {
            if (!a.quadPx.valid && !b.quadPx.valid) return a.boundsPx.left() < b.boundsPx.left();
            return QPointF::dotProduct(geometry(a).points[0], along) <
                   QPointF::dotProduct(geometry(b).points[0], along);
        });
    }
}

bool SameBits(double a, double b)
{
    return std::bit_cast<quint64>(a) == std::bit_cast<quint64>(b);
}
TextQuad MergeShape(const RecognizedWord& word)
{
    return word.quadPx.valid ? word.quadPx : TextQuad::FromRect(word.boundsPx);
}
} // namespace

// Everything cached here is a function of each word's boundsPx and quadPx
// bits alone; confidence, text, disposition and protection are always read
// from the output itself.
//
// Change detection: between calls the state holds a shallow copy (`shadow`)
// of the output. Qt containers detach before any modification while their
// buffer is shared, so anything done to the output through its API after a
// call - an assignment, append, erase, sort, swap, clear, or any non-const
// access to a row or word - gives it a different buffer, and the next call
// rebuilds. The copy is released before this call touches the output, so the
// merge's own edits never copy anything. Only a reference or pointer into the
// output taken before a call and written through after it would go unseen;
// every call can move words, so such references are not valid uses.
struct PaddleMergeContext::State
{
    struct Geometry {
        TextQuad shape;
        QRectF bounds;
        std::optional<double> area;
        double angle{}, height{};
        QPointF center;
        bool rowReady{};
    };
    struct Entry {
        Geometry g;
        int row{-1};         // row id, not output index
        int position{};      // index inside the row
        int x0{}, y0{}, x1{}, y1{};
        bool everywhere{};   // not in the grid: non-finite or very large bounds
        int centerCell{-1};  // row-search grid cell of the centre; -1: reach list
        double cx{}, cy{};   // the shape's centre
        quint32 seen{};
    };
    struct Row {
        std::vector<int> words; // entry ids in output order
        int index{};
        bool touched{};         // words erased or added during this call
        bool stable{};          // sorting it again cannot move a word
        quint32 seen{};
    };
    static constexpr qsizetype kMaximumRows = 1024;
    static constexpr std::size_t kMaximumWords = 8192;
    static constexpr int kMaximumGridCells = 256;
    static constexpr int kMaximumCellsPerWord = 64;
    static constexpr int kMaximumQueryCells = 1024;
    // Row search: a word joins a row only through a word whose centre lies
    // within its own extent plus this reach along the line (see RowCandidates).
    // Words wider plus taller than the reach, or with coordinates beyond the
    // bound, are visited by every row search.
    static constexpr double kRowReach = 512;
    static constexpr double kCoordinateBound = 0x1p24;

    const QVector<RecognizedLine>* owner{};
    QVector<RecognizedLine> shadow; // shares the output's buffer between calls
    QSize source;
    bool ready{};
    std::vector<Entry> entries;
    std::vector<int> freeEntries;
    std::vector<Row> rows;
    std::vector<int> freeRows;
    std::vector<int> order; // row id at each output index
    std::size_t words{};
    double cellWidth{1}, cellHeight{1};
    int cellsX{1}, cellsY{1};
    std::vector<std::vector<int>> cells;
    std::vector<int> everywhere;
    std::vector<std::vector<int>> centerCells;
    std::vector<int> reach; // entries every row search visits
    quint32 visit{}, rowVisit{};

    static Geometry MakeGeometry(const RecognizedWord& word)
    {
        Geometry result;
        result.shape = MergeShape(word);
        result.bounds = result.shape.Bounds();
        return result;
    }
    bool CellRange(const QRectF& r, int& x0, int& y0, int& x1, int& y1) const
    {
        // QRectF::intersects compares left/top with right()/bottom(), the
        // same sums used here. A shared interior point lies in a cell both
        // ranges contain, because division, floor and clamping are monotone.
        const double l = r.left(), t = r.top(), right = r.right(), b = r.bottom();
        if (!std::isfinite(l) || !std::isfinite(t) || !std::isfinite(right) || !std::isfinite(b))
            return false;
        const auto cell = [](double v, double size, int count) {
            return int(std::clamp(std::floor(v / size), 0.0, double(count - 1)));
        };
        x0 = cell(std::min(l, right), cellWidth, cellsX); x1 = cell(std::max(l, right), cellWidth, cellsX);
        y0 = cell(std::min(t, b), cellHeight, cellsY); y1 = cell(std::max(t, b), cellHeight, cellsY);
        return true;
    }
    static bool Moderate(double value) { return std::abs(value) <= kCoordinateBound; } // false for NaN
    int CellOf(double value, double size, int count) const
    {
        return int(std::clamp(std::floor(value / size), 0.0, double(count - 1)));
    }
    // Row-search cell of the shape's centre, or -1 when the word must be
    // visited by every row search (coordinates beyond the bound, or width
    // plus height beyond the reach).
    int CenterCell(const TextQuad& shape, double& cx, double& cy) const
    {
        double l = shape.points[0].x(), r = l, t = shape.points[0].y(), b = t;
        for (const QPointF& point : shape.points) {
            if (!Moderate(point.x()) || !Moderate(point.y())) return -1;
            l = std::min(l, point.x()); r = std::max(r, point.x());
            t = std::min(t, point.y()); b = std::max(b, point.y());
        }
        if ((r - l) + (b - t) > kRowReach) return -1;
        const QPointF center = shape.Center();
        cx = center.x(); cy = center.y();
        return CellOf(cy, cellHeight, cellsY) * cellsX + CellOf(cx, cellWidth, cellsX);
    }
    void Insert(int id)
    {
        auto& e = entries[std::size_t(id)];
        e.centerCell = CenterCell(e.g.shape, e.cx, e.cy);
        if (e.centerCell < 0) reach.push_back(id);
        else centerCells[std::size_t(e.centerCell)].push_back(id);
        e.everywhere = !CellRange(e.g.bounds, e.x0, e.y0, e.x1, e.y1) ||
            (e.x1 - e.x0 + 1) * (e.y1 - e.y0 + 1) > kMaximumCellsPerWord;
        if (e.everywhere) { everywhere.push_back(id); return; }
        for (int y = e.y0; y <= e.y1; ++y)
            for (int x = e.x0; x <= e.x1; ++x) cells[std::size_t(y * cellsX + x)].push_back(id);
    }
    static void Drop(std::vector<int>& list, int id)
    {
        const auto at = std::find(list.begin(), list.end(), id);
        *at = list.back();
        list.pop_back();
    }
    void Release(int id)
    {
        const auto& e = entries[std::size_t(id)];
        if (e.centerCell < 0) Drop(reach, id);
        else Drop(centerCells[std::size_t(e.centerCell)], id);
        if (e.everywhere) Drop(everywhere, id);
        else
            for (int y = e.y0; y <= e.y1; ++y)
                for (int x = e.x0; x <= e.x1; ++x) Drop(cells[std::size_t(y * cellsX + x)], id);
        freeEntries.push_back(id);
        --words;
    }
    int Add(int rowId, Geometry g)
    {
        int id;
        if (freeEntries.empty()) { id = int(entries.size()); entries.emplace_back(); }
        else { id = freeEntries.back(); freeEntries.pop_back(); }
        auto& e = entries[std::size_t(id)];
        e = Entry{};
        e.g = std::move(g);
        auto& row = rows[std::size_t(rowId)];
        e.row = rowId;
        e.position = int(row.words.size());
        row.words.push_back(id);
        Insert(id);
        ++words;
        return id;
    }
    int AddRow()
    {
        int id;
        if (freeRows.empty()) { id = int(rows.size()); rows.emplace_back(); }
        else { id = freeRows.back(); freeRows.pop_back(); }
        rows[std::size_t(id)] = Row{};
        rows[std::size_t(id)].index = int(order.size());
        order.push_back(id);
        return id;
    }
    void Rebuild(const QVector<RecognizedLine>& output, QSize size)
    {
        *this = State{};
        owner = &output;
        source = size;
        const int width = std::max(1, size.width()), height = std::max(1, size.height());
        cellWidth = std::max(64.0, width / double(kMaximumGridCells));
        cellHeight = std::max(64.0, height / double(kMaximumGridCells));
        cellsX = std::clamp(int(std::ceil(width / cellWidth)), 1, kMaximumGridCells);
        cellsY = std::clamp(int(std::ceil(height / cellHeight)), 1, kMaximumGridCells);
        cells.resize(std::size_t(cellsX) * std::size_t(cellsY));
        centerCells.resize(cells.size());
        for (const auto& line : output) {
            const int rowId = AddRow();
            for (const auto& word : line.words) Add(rowId, MakeGeometry(word));
        }
    }
    // Every cached word whose bounds intersect `query` (QRectF::intersects,
    // the merge's own first overlap test), in output order.
    void Candidates(const QRectF& query, std::vector<int>& result)
    {
        result.clear();
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!CellRange(query, x0, y0, x1, y1) || (x1 - x0 + 1) * (y1 - y0 + 1) > kMaximumQueryCells) {
            for (const int rowId : order)
                for (const int id : rows[std::size_t(rowId)].words)
                    if (query.intersects(entries[std::size_t(id)].g.bounds)) result.push_back(id);
            return;
        }
        if (++visit == 0) {
            for (auto& e : entries) e.seen = 0;
            visit = 1;
        }
        const auto take = [&](int id) {
            auto& e = entries[std::size_t(id)];
            if (e.seen == visit) return;
            e.seen = visit;
            if (query.intersects(e.g.bounds)) result.push_back(id);
        };
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (const int id : cells[std::size_t(y * cellsX + x)]) take(id);
        for (const int id : everywhere) take(id);
        std::sort(result.begin(), result.end(), [&](int a, int b) {
            const auto& ea = entries[std::size_t(a)];
            const auto& eb = entries[std::size_t(b)];
            const int ra = rows[std::size_t(ea.row)].index, rb = rows[std::size_t(eb.row)].index;
            return ra != rb ? ra < rb : ea.position < eb.position;
        });
    }
    // Output indices, ascending, of every row that can give the incoming word
    // a finite row distance; false means visit every row. `angle`, `height`
    // and `center` are the incoming word's prepared row geometry.
    //
    // A cached word O counts in rowDistance only if small > 0 and
    // small >= .5 * large, so large <= 2 * small <= 2h (h: this word's
    // height). Then dy <= .35 * large <= .7h puts O's centre within .7h of
    // this word's centre across the line, and gap <= 4 * small <= 4h makes
    // O's extent along the line meet [low - 4h, high + 4h]. O's centre lies
    // in the box of its points, so it is within that box's width plus height
    // (at most kRowReach for words in the grid) of the point of O realising
    // that meeting. The test below is that band with a pixel of slack, which
    // covers rounding many times over at coordinates within the bound. Rows
    // with no such word keep rowDistance's initial maximum and are never
    // chosen; the others are evaluated exactly, in output order.
    bool RowCandidates(const TextQuad& shape, double angle, double height, QPointF center,
                       std::vector<qsizetype>& result)
    {
        result.clear();
        if (!(height > 0) || !Moderate(height) || !std::isfinite(angle) ||
            !Moderate(center.x()) || !Moderate(center.y())) return false;
        const QPointF along(std::cos(angle), std::sin(angle)), across(-along.y(), along.x());
        double low = std::numeric_limits<double>::max(), high = std::numeric_limits<double>::lowest();
        for (const QPointF& point : shape.points) {
            if (!Moderate(point.x()) || !Moderate(point.y())) return false;
            const double value = QPointF::dotProduct(point, along);
            low = std::min(low, value); high = std::max(high, value);
        }
        constexpr double slack = 1;
        const double a0 = low - 4 * height - kRowReach - slack, a1 = high + 4 * height + kRowReach + slack;
        const double c = QPointF::dotProduct(center, across);
        const double c0 = c - .7 * height - slack, c1 = c + .7 * height + slack;
        double left = std::numeric_limits<double>::max(), right = std::numeric_limits<double>::lowest();
        double top = left, bottom = right;
        for (const double a : {a0, a1})
            for (const double b : {c0, c1}) {
                const double x = a * along.x() + b * across.x(), y = a * along.y() + b * across.y();
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
            }
        left -= slack; right += slack; top -= slack; bottom += slack;
        const int x0 = CellOf(left, cellWidth, cellsX), x1 = CellOf(right, cellWidth, cellsX);
        const int y0 = CellOf(top, cellHeight, cellsY), y1 = CellOf(bottom, cellHeight, cellsY);
        if ((x1 - x0 + 1) * (y1 - y0 + 1) > kMaximumQueryCells) return false;
        if (++rowVisit == 0) {
            for (auto& row : rows) row.seen = 0;
            rowVisit = 1;
        }
        const auto take = [&](int id) {
            auto& row = rows[std::size_t(entries[std::size_t(id)].row)];
            if (row.seen == rowVisit) return;
            row.seen = rowVisit;
            result.push_back(row.index);
        };
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (const int id : centerCells[std::size_t(y * cellsX + x)]) {
                    const auto& e = entries[std::size_t(id)];
                    const QPointF point(e.cx, e.cy);
                    const double a = QPointF::dotProduct(point, along), b = QPointF::dotProduct(point, across);
                    if (a >= a0 && a <= a1 && b >= c0 && b <= c1) take(id);
                }
        for (const int id : reach) take(id);
        std::sort(result.begin(), result.end());
        return true;
    }
};

PaddleMergeContext::PaddleMergeContext() = default;
PaddleMergeContext::~PaddleMergeContext() = default;
PaddleMergeContext::PaddleMergeContext(PaddleMergeContext&&) noexcept = default;
PaddleMergeContext& PaddleMergeContext::operator=(PaddleMergeContext&&) noexcept = default;

void PaddleMergeRecognition(QVector<RecognizedLine>& output, RecognizedLine line,
                             QRect region, QSize source)
{
    PaddleMergeContext context;
    PaddleMergeRecognition(output, std::move(line), region, source, context);
}

void PaddleMergeRecognition(QVector<RecognizedLine>& output, RecognizedLine line,
                             QRect region, QSize source, PaddleMergeContext& context)
{
    using State = PaddleMergeContext::State;
    if (!context.state_) context.state_ = std::make_unique<State>();
    State& s = *context.state_;
    const bool reuse = s.ready && s.owner == &output && s.source == source &&
        s.shadow.size() == output.size() && s.shadow.constData() == std::as_const(output).constData();
    s.ready = false;  // an exception below leaves nothing to trust
    s.shadow = {};    // release before any write, so the merge never detaches
    std::size_t total = s.words;
    bool fits = reuse;
    if (!reuse) {
        fits = output.size() <= State::kMaximumRows;
        total = 0;
        for (qsizetype index = 0; fits && index < output.size(); ++index) {
            total += std::size_t(std::as_const(output)[index].words.size());
            fits = total <= State::kMaximumWords;
        }
    }
    if (!fits || std::size_t(line.words.size()) > State::kMaximumWords - total) {
        context.state_.reset();
        MergeWithoutContext(output, std::move(line), region, source);
        return;
    }
    if (!reuse) s.Rebuild(std::as_const(output), source);

    // From here on this is MergeWithoutContext's procedure with the cached
    // geometry; comments mark where the context replaces a full scan.
    const bool detail = region.size() != source;
    const auto geometry = [](const RecognizedWord& word) { return MergeShape(word); };
    using Geometry = State::Geometry;
    const auto area = [](Geometry& value) {
        if (!value.area) value.area = QuadArea(value.shape);
        return *value.area;
    };
    const auto prepareRow = [](Geometry& value) {
        if (value.rowReady) return;
        value.angle = QuadAngle(value.shape);
        value.height = QuadHeight(value.shape);
        value.center = value.shape.Center();
        value.rowReady = true;
    };
    const auto overlaps = [&](const RecognizedWord& a, const RecognizedWord& b,
                              Geometry& ga, Geometry& gb) {
        if (!ga.bounds.intersects(gb.bounds)) return false;
        const double areaA = area(ga), areaB = area(gb);
        const double smaller = std::min(areaA, areaB);
        if (smaller <= 0) return false;
        const double intersection = QuadIntersectionArea(ga.shape, gb.shape);
        if (intersection >= .7 * smaller) return true;
        if (intersection < .65 * smaller) return false;
        const QString& shortText = a.text.size() < b.text.size() ? a.text : b.text;
        const QString& longText = a.text.size() < b.text.size() ? b.text : a.text;
        const bool fragment = !shortText.isEmpty() && shortText.size() < longText.size() &&
            smaller < .35 * std::max(areaA, areaB) &&
            (longText.startsWith(shortText) || longText.endsWith(shortText));
        return fragment;
    };
    const auto rowDistance = [&](qsizetype index, Geometry& wordGeometry) {
        double best = std::numeric_limits<double>::max();
        prepareRow(wordGeometry);
        const TextQuad& wordShape = wordGeometry.shape;
        const double angle = wordGeometry.angle;
        const QPointF along(std::cos(angle), std::sin(angle)), across(-along.y(), along.x());
        const auto extent = [&](const TextQuad& quad) {
            double low = std::numeric_limits<double>::max(), high = std::numeric_limits<double>::lowest();
            for (const QPointF& point : quad.points) {
                const double t = QPointF::dotProduct(point, along);
                low = std::min(low, t); high = std::max(high, t);
            }
            return std::pair{low, high};
        };
        const auto [wordLow, wordHigh] = extent(wordShape);
        for (const int id : s.rows[std::size_t(s.order[std::size_t(index)])].words) {
            auto& oldGeometry = s.entries[std::size_t(id)].g;
            prepareRow(oldGeometry);
            const TextQuad& oldShape = oldGeometry.shape;
            double turn = std::abs(oldGeometry.angle - angle);
            turn = std::fmod(turn, std::numbers::pi);
            if (std::min(turn, std::numbers::pi - turn) > .175) continue;
            const double small = std::min(oldGeometry.height, wordGeometry.height);
            const double large = std::max(oldGeometry.height, wordGeometry.height);
            const double dy = std::abs(QPointF::dotProduct(oldGeometry.center - wordGeometry.center, across));
            const auto [oldLow, oldHigh] = extent(oldShape);
            const double gap = std::max({0.0, oldLow - wordHigh, wordLow - oldHigh});
            if (small > 0 && small >= .5 * large && dy <= .35 * large && gap <= 4 * small)
                best = std::min(best, gap + 4 * dy);
        }
        return best;
    };
    const auto inheritableProtection = [](const RecognizedWord& value) {
        return !value.protectedBoundsPx.isEmpty() &&
            value.disposition==TokenDisposition::kInvalidGeometry &&
            (value.fromRibbon || value.ribbonErrorPx<0);
    };
    qsizetype preferredLine = -1;
    std::vector<int> candidates, hits;
    std::vector<qsizetype> rowSearch;
    for (auto word : line.words) {
        constexpr double margin = kDetailCropEdgeMarginPx;
        if (detail && ((region.left() > 0 && word.boundsPx.left() < margin) ||
            (region.top() > 0 && word.boundsPx.top() < margin) ||
            (region.x() + region.width() < source.width() && word.boundsPx.right() > region.width() - margin) ||
            (region.y() + region.height() < source.height() && word.boundsPx.bottom() > region.height() - margin))) continue;
        word.boundsPx.translate(region.topLeft());
        if (!word.inkSearchPx.isEmpty()) word.inkSearchPx.translate(region.topLeft());
        if (!word.protectedBoundsPx.isEmpty()) word.protectedBoundsPx.translate(region.topLeft());
        if (word.quadPx.valid) word.quadPx = word.quadPx.Translated(region.topLeft());
        if (word.inkSearchQuadPx.valid) word.inkSearchQuadPx = word.inkSearchQuadPx.Translated(region.topLeft());
        if (word.fromRibbon) word.ribbonReadingPointPx += region.topLeft();
        auto wordGeometry = State::MakeGeometry(word);
        bool use = true;
        qsizetype dropLine = -1, dropWord = -1;
        QRectF inheritedProtection;
        qsizetype target = -1;
        // Words whose bounds miss this word's bounds fail the first overlap
        // test, so only the intersecting ones are visited, in output order.
        s.Candidates(wordGeometry.bounds, candidates);
        hits.clear();
        for (const int id : candidates) {
            auto& entry = s.entries[std::size_t(id)];
            const qsizetype index = s.rows[std::size_t(entry.row)].index, wi = entry.position;
            const auto& old = std::as_const(output)[index].words[wi];
            if (!overlaps(word, old, wordGeometry, entry.g)) continue;
            hits.push_back(id);
            if (target < 0) target = index;
            if (word.confidence < old.confidence ||
                (word.disposition != TokenDisposition::kCandidate && old.disposition == TokenDisposition::kCandidate)) {
                use = false;
                dropLine = index;
                dropWord = wi;
                break;
            }
        }
        if (target >= 0) preferredLine = target;
        if (!use) {
            if (inheritableProtection(word) && dropLine >= 0) {
                auto& winner = output[dropLine].words[dropWord];
                winner.protectedBoundsPx = winner.protectedBoundsPx.isEmpty()
                    ? word.protectedBoundsPx
                    : winner.protectedBoundsPx.united(word.protectedBoundsPx);
                winner.disposition=TokenDisposition::kInvalidGeometry;
                winner.ribbonErrorPx=-1;
            }
            continue;
        }
        if (target < 0 && preferredLine >= 0 &&
            rowDistance(preferredLine, wordGeometry) < std::numeric_limits<double>::max())
            target = preferredLine;
        qsizetype emptyLine = -1;
        if (target < 0) {
            double best = std::numeric_limits<double>::max();
            for (qsizetype index = 0; index < qsizetype(s.order.size()); ++index)
                if (s.rows[std::size_t(s.order[std::size_t(index)])].words.empty()) emptyLine = index;
            // Only rows holding a word that can count are visited; every
            // other non-empty row would return the initial maximum.
            prepareRow(wordGeometry);
            if (!s.RowCandidates(wordGeometry.shape, wordGeometry.angle, wordGeometry.height,
                                 wordGeometry.center, rowSearch)) {
                rowSearch.clear();
                for (qsizetype index = 0; index < qsizetype(s.order.size()); ++index)
                    if (!s.rows[std::size_t(s.order[std::size_t(index)])].words.empty()) rowSearch.push_back(index);
            }
            for (const qsizetype index : rowSearch) {
                const double distance = rowDistance(index, wordGeometry);
                if (distance < best) {
                    best = distance;
                    target = index;
                }
            }
        }
        if (target < 0) {
            if (emptyLine >= 0) target = emptyLine;
            else {
                if (output.size() >= 1024) continue;
                target = output.size();
                output.push_back({});
                s.AddRow();
            }
        }
        // Erase exactly the overlapping words found above, in output order.
        for (const int id : hits) {
            const auto& entry = s.entries[std::size_t(id)];
            const auto& old = std::as_const(output)[s.rows[std::size_t(entry.row)].index].words[entry.position];
            if (inheritableProtection(old))
                inheritedProtection = inheritedProtection.isEmpty()
                    ? old.protectedBoundsPx
                    : inheritedProtection.united(old.protectedBoundsPx);
        }
        for (std::size_t h = 0; h < hits.size();) {
            const int rowId = s.entries[std::size_t(hits[h])].row;
            std::size_t end = h;
            while (end < hits.size() && s.entries[std::size_t(hits[end])].row == rowId) ++end;
            auto& row = s.rows[std::size_t(rowId)];
            auto& words = output[row.index].words;
            qsizetype kept = 0;
            std::size_t next = h;
            for (qsizetype wi = 0; wi < words.size(); ++wi) {
                if (next < end && s.entries[std::size_t(hits[next])].position == wi) {
                    s.Release(hits[next++]);
                    continue;
                }
                if (kept != wi) {
                    words[kept] = std::move(words[wi]);
                    row.words[std::size_t(kept)] = row.words[std::size_t(wi)];
                    s.entries[std::size_t(row.words[std::size_t(kept)])].position = int(kept);
                }
                ++kept;
            }
            words.erase(words.begin() + kept, words.end());
            row.words.resize(std::size_t(kept));
            row.touched = true;
            h = end;
        }
        if (!inheritedProtection.isEmpty()) {
            word.protectedBoundsPx = word.protectedBoundsPx.isEmpty()
                ? inheritedProtection
                : word.protectedBoundsPx.united(inheritedProtection);
            word.disposition=TokenDisposition::kInvalidGeometry;
            word.ribbonErrorPx=-1;
        }
        output[target].words.push_back(std::move(word));
        const int rowId = s.order[std::size_t(target)];
        s.rows[std::size_t(rowId)].touched = true;
        s.Add(rowId, std::move(wordGeometry));
        preferredLine = target;
    }
    bool emptied = false;
    for (const int rowId : s.order) emptied = emptied || s.rows[std::size_t(rowId)].words.empty();
    if (emptied) {
        output.erase(std::remove_if(output.begin(), output.end(), [](const auto& l) {
            return l.words.isEmpty();
        }), output.end());
        std::size_t kept = 0;
        for (const int rowId : s.order) {
            auto& row = s.rows[std::size_t(rowId)];
            if (row.words.empty()) { s.freeRows.push_back(rowId); continue; }
            row.index = int(kept);
            s.order[kept++] = rowId;
        }
        s.order.resize(kept);
    }
    // Reading order along the row's own baseline (left edge when upright).
    std::vector<int> permutation, ids;
    for (qsizetype index = 0; index < output.size(); ++index) {
        const int rowId = s.order[std::size_t(index)];
        auto& row = s.rows[std::size_t(rowId)];
        // An unchanged row that was left sorted under a strict weak order
        // whose baseline its first word still gives: sorting is the identity.
        if (!row.touched && row.stable) continue;
        row.touched = false;
        auto& words = output[index].words;
        const double angle = QuadAngle(geometry(words.first()));
        const QPointF along(std::cos(angle), std::sin(angle));
        const auto before = [&](const RecognizedWord& a, const RecognizedWord& b) {
            if (!a.quadPx.valid && !b.quadPx.valid) return a.boundsPx.left() < b.boundsPx.left();
            return QPointF::dotProduct(geometry(a).points[0], along) <
                   QPointF::dotProduct(geometry(b).points[0], along);
        };
        // With finite bounded sort keys the comparison is a strict weak
        // order (mixed rectangle/quad rows only on an exactly upright
        // baseline, where both forms compare the left edge), so every stable
        // sort yields the same unique permutation and an index sort is exact.
        bool strict = std::isfinite(along.x()) && std::isfinite(along.y());
        bool anyQuad = false, anyRect = false;
        for (const auto& word : words) {
            if (!strict) break;
            const QPointF key = word.quadPx.valid ? word.quadPx.points[0] : word.boundsPx.topLeft();
            strict = std::abs(key.x()) < 0x1p500 && std::abs(key.y()) < 0x1p500;
            (word.quadPx.valid ? anyQuad : anyRect) = true;
        }
        strict = strict && (!(anyQuad && anyRect) || (along.x() == 1 && along.y() == 0));
        if (!strict) {
            // Exactly the original call; the row's cache follows the result.
            std::stable_sort(words.begin(), words.end(), before);
            const auto old = row.words;
            row.words.clear();
            for (const int id : old) s.Release(id);
            for (const auto& word : words) s.Add(rowId, State::MakeGeometry(word));
            row.stable = false;
            continue;
        }
        permutation.resize(std::size_t(words.size()));
        for (std::size_t i = 0; i < permutation.size(); ++i) permutation[i] = int(i);
        std::stable_sort(permutation.begin(), permutation.end(), [&](int a, int b) {
            return before(words[a], words[b]);
        });
        bool moved = false;
        for (std::size_t i = 0; i < permutation.size() && !moved; ++i) moved = permutation[i] != int(i);
        if (moved) {
            QVector<RecognizedWord> sorted;
            sorted.reserve(words.size());
            ids.resize(permutation.size());
            for (std::size_t i = 0; i < permutation.size(); ++i) {
                sorted.push_back(std::move(words[permutation[i]]));
                ids[i] = row.words[std::size_t(permutation[i])];
                s.entries[std::size_t(ids[i])].position = int(i);
            }
            words = std::move(sorted);
            row.words.assign(ids.begin(), ids.end());
        }
        row.stable = SameBits(QuadAngle(geometry(words.first())), angle);
    }
    s.shadow = output;
    s.ready = true;
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
                                 const std::atomic<bool> &cancelled, bool curvedRibbon)
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
        ribbon::Geometry curve;
        if (curvedRibbon && u1 - u0 >= 24 && component.size() <= ribbon::kMaximumFitPixels &&
            (x1-x0+1)*(y1-y0+1) <= ribbon::kMaximumFitPixels && w >= h) {
            ribbon::Bin bins[ribbon::kBins]{};
            for (int p : component) {
                const double u=(p%width)*c+(p/width)*s, v=-(p%width)*s+(p/width)*c;
                ribbon::Add(bins, (u-u0)/std::max(1.0,u1-u0), v);
            }
            curve=ribbon::Fit(bins,u0,u1,c,s,double(source.width())/width,
                               double(source.height())/height,source.width(),source.height());
        }
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
        if(curve.unsupported) {
            TextQuad envelope;envelope.points=q.points;envelope.valid=true;const auto r=envelope.Bounds();
            curve.protectLeft=r.left();curve.protectTop=r.top();curve.protectRight=r.right();curve.protectBottom=r.bottom();
        }
        q.ribbon=curve;
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
    if (quad.ribbon.valid && !quad.ribbon.Inside(image.width(),image.height())) return {};
    const auto source = image.convertToFormat(QImage::Format_ARGB32);
    std::vector<float> crop(static_cast<std::size_t>(width) * 48 * 3, 0.f);
    const int valid = std::min(width, validWidth);
    if (quad.ribbon.valid) {
        // Per-column centerline terms, evaluated once per crop with the same
        // double operation order as ribbon::Geometry::At.
        const auto& g = quad.ribbon;
        std::vector<double> baseX(valid), baseY(valid), center(valid), half(valid);
        for (int x = 0; x < valid; ++x) {
            const double u = (x + .5f) / valid;
            const double t = g.start + (g.end - g.start) * u;
            center[x] = g.a + t * (g.b + t * g.c);
            half[x] = g.h0 + g.h1 * t;
            baseX[x] = g.ox + g.ux * t;
            baseY[x] = g.oy + g.uy * t;
        }
        for (int y = 0; y < 48; ++y) {
            const double v = (y + .5f) / 48;
            const double side = 2 * v - 1;
            for (int x = 0; x < valid; ++x) {
                const double cross = center[x] + side * half[x];
                const double px = baseX[x] + g.vx * cross;
                const double py = baseY[x] + g.vy * cross;
                for (int ch = 0; ch < 3; ++ch)
                    crop[ch * width * 48 + y * width + x] =
                        (Sample(source, static_cast<float>(px) - .5f,
                                static_cast<float>(py) - .5f, ch) / 255 - .5f) * 2;
            }
        }
        return crop;
    }
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
    if (quad.ribbon.valid) {
        // Decode in u coordinates with the exact established CTC segmentation;
        // only then map each span back to the original analysis image. The
        // flattened recognition tensor never becomes a reference or ink image.
        const double width=PaddleRecognitionWidth(quad), height=PaddleRecognitionHeight(quad);
        if (!std::isfinite(width)||!std::isfinite(height)||width<2||height<2) return {};
        PaddleQuad local{{QPointF(0,0),QPointF(width,0),QPointF(width,height),QPointF(0,height)},
                         quad.score,quad.internalEdges};
        auto line=PaddleDecodeTokens(tokens,steps,local,dictionary);
        for (auto& word : line.words) {
            const double u0=word.quadPx.points[0].x()/width, u1=word.quadPx.points[1].x()/width;
            const double s0=word.inkSearchQuadPx.points[0].x()/width, s1=word.inkSearchQuadPx.points[1].x()/width;
            const auto chord=[&](double a,double b) {
                TextQuad q; q.valid=true;
                q.points={At(quad,a,0),At(quad,b,0),At(quad,b,1),At(quad,a,1)}; return q;
            };
            const auto envelope=[&](double a,double b) {
                // The whole quadratic curve lies within its chord's exact
                // source-pixel sagitta. This AABB protects unsupported ink too.
                const auto q=chord(a,b); const double error=quad.ribbon.Error(a,b);
                return q.Bounds().adjusted(-error,-error,error,error);
            };
            word.quadPx=chord(u0,u1); word.inkSearchQuadPx=chord(s0,s1);
            word.boundsPx=word.quadPx.Bounds(); word.inkSearchPx=word.inkSearchQuadPx.Bounds();
            word.protectedBoundsPx=envelope(s0,s1);
            word.fromRibbon=true; word.ribbonErrorPx=quad.ribbon.Error(u0,u1);
            word.ribbonReadingPointPx=At(quad,(u0+u1)*.5,.5);
            // The search frame is a chord too. Refuse its error as well so
            // fitting cannot cross the geometry bound after a one-step widen.
            const double limit=std::min(ribbon::kMaximumWordErrorPx,
                                       ribbon::kMaximumWordErrorHeight*QuadHeight(word.quadPx));
            if (quad.ribbon.Error(s0,s1)>limit || QuadArea(word.quadPx)<=0)
                word.disposition=TokenDisposition::kInvalidGeometry;
        }
        return line;
    }
    RecognizedLine line;
    if (steps <= 0)
        return line;
    QString word;
    int first = 0, last = 0;
    double confidence = 0;
    int count = 0;
    // Fractions along the line quad, parallel to line.words: CTC span and
    // its one-step-widened search span. Oriented words are built from these.
    struct Span { double x0, x1, search0, search1; };
    QVector<Span> spans;
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
            if (!((quad.internalEdges&1 && x0<context) || (quad.internalEdges&2 && x1>1-context))) {
            line.words.push_back({word, bounds, true,
                                  QRectF(searchLeft, top, searchRight - searchLeft, bottom - top),
                                  meanConfidence >= .7 ? TokenDisposition::kCandidate
                                                       : TokenDisposition::kLowConfidence,
                                  meanConfidence, bounds});
            spans.push_back({x0, x1, searchX0, searchX1});
            }
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
        // The same neighbour bound, along the detector's own baseline. A
        // tilted line's word keeps its rotated shape through fitting/paint.
        const Span& span = spans[i];
        const double searchLeft = i > 0 ? std::max(span.search0, (spans[i - 1].x1 + span.x0) * .5) : span.search0;
        const double searchRight = i + 1 < spans.size() ? std::min(span.search1, (span.x1 + spans[i + 1].x0) * .5)
                                                        : span.search1;
        const auto quadAt = [&](double a, double b) {
            TextQuad result;
            result.points = {At(quad, a, 0), At(quad, b, 0), At(quad, b, 1), At(quad, a, 1)};
            result.valid = b > a;
            return result;
        };
        current.quadPx = quadAt(span.x0, span.x1);
        current.inkSearchQuadPx = quadAt(searchLeft, std::max(searchLeft, searchRight));
        if (!current.inkSearchQuadPx.valid) current.inkSearchQuadPx = current.quadPx;
    }
    if (quad.ribbon.unsupported) {
        TextQuad detector;detector.points=quad.points;detector.valid=true;
        for(auto& word:line.words) {
            word.disposition=TokenDisposition::kInvalidGeometry;
            word.protectedBoundsPx=quad.ribbon.protectRight>quad.ribbon.protectLeft &&
                quad.ribbon.protectBottom>quad.ribbon.protectTop
                ? QRectF(QPointF(quad.ribbon.protectLeft,quad.ribbon.protectTop),
                         QPointF(quad.ribbon.protectRight,quad.ribbon.protectBottom)) : detector.Bounds();
            word.fromRibbon=true;word.ribbonErrorPx=-1;word.ribbonReadingPointPx=word.quadPx.Center();
        }
    }
    return line;
}

} // namespace okuflow::uct
