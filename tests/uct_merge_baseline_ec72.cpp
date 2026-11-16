// Exact ec7282d PaddleMergeRecognition body (the merge before the per-read
// context, including refused-ribbon protection inheritance, which the bb53
// oracle predates); only the function name differs. It calls the production
// QuadArea/QuadIntersectionArea/QuadAngle/QuadHeight, which GeometryControls
// compares with the frozen bb53 geometry. Do not refresh from the candidate.
#include "uct_merge_baseline_ec72.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>
namespace okuflow::uct::merge_replay {
void FrozenEc72PaddleMergeRecognition(QVector<RecognizedLine>& output, RecognizedLine line,
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
} // namespace okuflow::uct::merge_replay
