// Exact bb53ac884c24d15f0c77b2dc43cace0e8e650c24 bodies; Frozen names only.
#include "uct_merge_baseline_bb53.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
namespace okuflow::uct::merge_replay {
void FrozenPaddleMergeRecognition(QVector<RecognizedLine>& output, RecognizedLine line,
                             QRect region, QSize source)
{
    const bool detail = region.size() != source;
    // Rotated words compare their detector shapes. Bounding boxes of
    // neighbouring tilted lines overlap even where the text does not.
    const auto geometry = [](const RecognizedWord& word) {
        return word.quadPx.valid ? word.quadPx : TextQuad::FromRect(word.boundsPx);
    };
    const auto overlaps = [&](const RecognizedWord& a, const RecognizedWord& b) {
        const TextQuad ga = geometry(a), gb = geometry(b);
        const double areaA = FrozenQuadArea(ga), areaB = FrozenQuadArea(gb);
        const double smaller = std::min(areaA, areaB);
        if (smaller <= 0) return false;
        const double intersection = FrozenQuadIntersectionArea(ga, gb);
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
    const auto rowDistance = [&](const RecognizedLine& row, const RecognizedWord& word) {
        double best = std::numeric_limits<double>::max();
        const TextQuad wordShape = geometry(word);
        const double angle = QuadAngle(wordShape);
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
        for (const auto& old : row.words) {
            const TextQuad oldShape = geometry(old);
            // Differently tilted text is a different line even when the
            // bounding boxes of the two rotated rows interleave.
            double turn = std::abs(QuadAngle(oldShape) - angle);
            turn = std::fmod(turn, std::numbers::pi);
            if (std::min(turn, std::numbers::pi - turn) > .175) continue;
            const double small = std::min(QuadHeight(oldShape), QuadHeight(wordShape));
            const double large = std::max(QuadHeight(oldShape), QuadHeight(wordShape));
            const double dy = std::abs(QPointF::dotProduct(oldShape.Center() - wordShape.Center(), across));
            const auto [oldLow, oldHigh] = extent(oldShape);
            const double gap = std::max({0.0, oldLow - wordHigh, wordLow - oldHigh});
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
