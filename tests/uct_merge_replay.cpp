#include "uct_merge_baseline_bb53.hpp"
#include "uct_merge_baseline_ec72.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTransform>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace okuflow::uct;
namespace {
using Lines = QVector<RecognizedLine>;
using Merge = std::function<void(Lines&, const RecognizedLine&, QRect, QSize)>;
// Snapshot/frozen-input format: 2 adds the ribbon fields that protection
// inheritance reads and writes.
constexpr quint32 kFormat = 2;
constexpr QSize scene{4096, 4096};
struct Call { RecognizedLine line; QRect region{QPoint(), scene}; QSize source{scene}; };
struct Case { QString name; Lines initial; QVector<Call> calls; bool timed{true}; };

void Require(bool value, const QString& message)
{
    if (!value) throw std::runtime_error(message.toStdString());
}
void Bits(QDataStream& out, double value) { out << std::bit_cast<quint64>(value); }
void Rect(QDataStream& out, const QRectF& rect)
{
    Bits(out, rect.x()); Bits(out, rect.y()); Bits(out, rect.width()); Bits(out, rect.height());
}
void Quad(QDataStream& out, const TextQuad& quad)
{
    out << quad.valid;
    for (const auto& p : quad.points) { Bits(out, p.x()); Bits(out, p.y()); }
}
QByteArray Snapshot(const Lines& lines, quint32 format = kFormat)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << quint64(lines.size());
    for (const auto& line : lines) {
        out << quint64(line.words.size());
        for (const auto& word : line.words) {
            out << word.text;
            Rect(out, word.boundsPx);
            out << word.paddedBounds;
            Rect(out, word.inkSearchPx);
            out << quint8(word.disposition);
            Bits(out, word.confidence);
            Rect(out, word.protectedBoundsPx);
            out << word.colors.has_value();
            if (word.colors) out << word.colors->foreground << word.colors->background;
            Quad(out, word.quadPx); Quad(out, word.inkSearchQuadPx);
            out << word.inkFitted;
            if (format >= 2) {
                out << word.fromRibbon; Bits(out, word.ribbonErrorPx);
                Bits(out, word.ribbonReadingPointPx.x()); Bits(out, word.ribbonReadingPointPx.y());
            }
        }
    }
    return bytes;
}
double ReadBits(QDataStream& in)
{
    quint64 bits{}; in >> bits; return std::bit_cast<double>(bits);
}
QRectF ReadRect(QDataStream& in)
{
    const auto x=ReadBits(in),y=ReadBits(in),w=ReadBits(in),h=ReadBits(in);
    return {x,y,w,h};
}
TextQuad ReadQuad(QDataStream& in)
{
    TextQuad quad; in >> quad.valid;
    for(auto& p:quad.points) { const auto x=ReadBits(in),y=ReadBits(in); p={x,y}; }
    return quad;
}
Lines Restore(const QByteArray& bytes, quint32 format = kFormat)
{
    QDataStream in(bytes); in.setVersion(QDataStream::Qt_6_0);
    quint64 rows{}; in >> rows; Require(rows<=2048,"Frozen row bound exceeded");
    Lines lines; quint64 total=0;
    for(quint64 r=0;r<rows;++r) {
        quint64 count{}; in >> count; total+=count;
        Require(count<=65536 && total<=65536,"Frozen word bound exceeded");
        RecognizedLine row;
        for(quint64 w=0;w<count;++w) {
            RecognizedWord word; quint8 disposition{}; bool colors{};
            in >> word.text; word.boundsPx=ReadRect(in); in >> word.paddedBounds;
            word.inkSearchPx=ReadRect(in); in >> disposition;
            word.disposition=TokenDisposition(disposition); word.confidence=ReadBits(in);
            word.protectedBoundsPx=ReadRect(in); in >> colors;
            if(colors) { LineColors value; in >> value.foreground >> value.background; word.colors=value; }
            word.quadPx=ReadQuad(in); word.inkSearchQuadPx=ReadQuad(in); in >> word.inkFitted;
            if(format>=2) {
                in >> word.fromRibbon; word.ribbonErrorPx=ReadBits(in);
                const auto x=ReadBits(in),y=ReadBits(in); word.ribbonReadingPointPx={x,y};
            }
            row.words.push_back(word);
        }
        lines.push_back(row);
    }
    Require(in.status()==QDataStream::Ok && in.atEnd(),"Frozen snapshot truncated/trailing");
    return lines;
}
RecognizedWord Word(QString text, double x, double y, double w = 24, double h = 12,
                    double confidence = .9, double angle = 0)
{
    RecognizedWord word;
    word.text = std::move(text);
    word.boundsPx = {x, y, w, h};
    word.confidence = confidence;
    word.paddedBounds = true;
    word.inkSearchPx = word.boundsPx.adjusted(-1, -2, 1, 2);
    word.protectedBoundsPx = word.boundsPx.adjusted(-2, -3, 2, 3);
    word.colors = LineColors{QColor(7, 19, 31, 221), QColor(231, 245, 253)};
    if (angle != 0) {
        QTransform transform;
        transform.translate(x, y); transform.rotateRadians(angle); transform.translate(-x, -y);
        word.quadPx = TextQuad::FromRect(word.boundsPx);
        for (auto& p : word.quadPx.points) p = transform.map(p);
        word.inkSearchQuadPx = word.quadPx;
        word.boundsPx = word.quadPx.Bounds();
    }
    return word;
}
void Add(Case& c, QVector<RecognizedWord> words, QRect region = QRect(QPoint(), scene),
         QSize source = scene)
{
    c.calls.push_back({{std::move(words)}, region, source});
}
Case Page(const QString& name, int count, int columns, double angle)
{
    Case c{name};
    QVector<RecognizedLine> rows;
    for (int i = 0; i < count; ++i) {
        const int row = i / columns;
        if (row == rows.size()) rows.push_back({});
        auto word = Word(QString::number(i) + "-token", 40 + (i % columns) * 31,
                         40 + row * 21, 25, 12, .8 + (i % 13) * .01, angle);
        if (i % 11 == 0) word.disposition = TokenDisposition::kHighRisk;
        rows[row].words.push_back(word);
    }
    // These are generated crop-like call sequences, not captured detector calls.
    for (const auto& row : rows) Add(c, row.words);
    for (auto row : rows) {
        for (auto& word : row.words) { word.confidence = .96; word.inkFitted = true; }
        Add(c, row.words);
    }
    return c;
}
// Deterministic generator for read-shaped call sequences: one page of text
// lines, every detector region of a full read in production order (whole
// frame, then the overlapping tiles), one call per recognition window in
// region-local pixels. Tile passes re-read the same words with jittered
// geometry and confidence, cut words at tile edges (some inside the edge
// margin, some just outside it), and split long lines into overlapping
// windows. Constructed, not captured: it stresses one output across hundreds
// of calls the way a dense read does, it is not detector ground truth.
struct ReadLikeOptions {
    QSize source;
    quint64 seed{1};
    double tilt{};          // page rotation in radians
    int columns{1};         // independent text columns
    bool ribbons{};         // some refused-ribbon words carry inheritable protection
    double typeScale{1};
    double quadShare{1};    // words with a detector quad; the rest are rectangles
};
Case ReadLike(const QString& name, const ReadLikeOptions& o)
{
    Case c{name};
    quint64 state = o.seed * 0x9e3779b97f4a7c15ULL + 1;
    const auto next = [&] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return double(state >> 11) * 0x1.0p-53;
    };
    struct TextWord { QString text; QRectF box; };
    QVector<QVector<TextWord>> page;
    const double columnWidth = (o.source.width() - 60.0) / o.columns;
    for (int column = 0; column < o.columns; ++column) {
        double y = 30 + 7 * column;
        int serial = 0;
        while (y < o.source.height() - 80) {
            const bool title = next() < .06;
            const double h = (title ? 34 + 20 * next() : 13 + 9 * next()) * o.typeScale;
            QVector<TextWord> line;
            double x = 30 + column * columnWidth + 12 * next();
            const double right = 30 + (column + 1) * columnWidth - 20;
            const double end = right - (next() < .2 ? columnWidth * next() * .6 : 0);
            while (true) {
                const int letters = 2 + int(next() * 9);
                const double w = h * (.45 * letters + .2 * next());
                if (x + w > end) break;
                QString text = QString::number(column) + "c" + QString::number(page.size()) + "w";
                while (text.size() < letters) text += QChar(u'a' + (serial++ % 26));
                line.push_back({text, {x, y, w, h}});
                x += w + h * (.35 + .25 * next());
            }
            if (!line.isEmpty()) page.push_back(line);
            y += h * (1.45 + .4 * next());
        }
    }
    const QPointF pivot(o.source.width() / 2.0, o.source.height() / 2.0);
    const auto place = [&](const QRectF& box, double jitter) {
        RecognizedWord word;
        const QRectF moved = box.translated((next() - .5) * jitter, (next() - .5) * jitter)
            .adjusted(0, 0, (next() - .5) * jitter, (next() - .5) * jitter);
        word.boundsPx = moved;
        if (o.tilt != 0 || next() < o.quadShare) {
            QTransform transform;
            transform.translate(pivot.x(), pivot.y()); transform.rotateRadians(o.tilt);
            transform.translate(-pivot.x(), -pivot.y());
            word.quadPx = TextQuad::FromRect(moved);
            for (auto& p : word.quadPx.points) p = transform.map(p);
            word.inkSearchQuadPx = word.quadPx;
            word.boundsPx = word.quadPx.Bounds();
        }
        word.inkSearchPx = word.boundsPx.adjusted(-1, -2, 1, 2);
        word.protectedBoundsPx = word.boundsPx.adjusted(-2, -3, 2, 3);
        word.paddedBounds = true;
        word.colors = LineColors{QColor(10, 12, 14), QColor(240, 238, 230)};
        return word;
    };
    const auto local = [](RecognizedWord word, QPoint origin) {
        const QPointF offset(-origin.x(), -origin.y());
        word.boundsPx.translate(offset); word.inkSearchPx.translate(offset);
        word.protectedBoundsPx.translate(offset);
        if (word.quadPx.valid) word.quadPx = word.quadPx.Translated(offset);
        if (word.inkSearchQuadPx.valid) word.inkSearchQuadPx = word.inkSearchQuadPx.Translated(offset);
        return word;
    };
    const auto regions = PaddleDetectionRegions(o.source);
    for (const QRect& region : regions) {
        const bool whole = region.size() == o.source;
        // Tiles see the page at full scale: steadier boxes, more confidence.
        const double jitter = whole ? 1.6 : .7;
        for (const auto& line : page) {
            // The detector's view of this line inside the region; a few
            // detections stop short of the tile edge, leaving a fragment
            // just outside the merge's edge margin.
            const double inset = !whole && next() < .25 ? 9 + 12 * next() : 0;
            const QRectF view = QRectF(region).adjusted(inset, 0, -inset, 0);
            QVector<RecognizedWord> words;
            for (const auto& source : line) {
                if (!view.intersects(source.box) || source.box.top() < view.top() ||
                    source.box.bottom() > view.bottom()) continue;
                QRectF box = source.box;
                QString text = source.text;
                if (!view.contains(box)) {
                    const QRectF cut = box.intersected(view);
                    const qsizetype keep = qsizetype(text.size() * cut.width() / box.width());
                    if (keep < 1) continue;
                    text = cut.left() > box.left() ? text.right(keep) : text.left(keep);
                    box = cut;
                }
                auto word = place(box, jitter);
                word.text = text;
                word.confidence = (whole ? .62 : .78) + (whole ? .36 : .21) * next();
                const double roll = next();
                if (roll < .04) word.disposition = TokenDisposition::kLowConfidence;
                else if (roll < .07) word.disposition = TokenDisposition::kHighRisk;
                else if (o.ribbons && roll < .13) {
                    word.fromRibbon = true;
                    word.disposition = TokenDisposition::kInvalidGeometry;
                    word.ribbonErrorPx = 1 + next();
                    word.protectedBoundsPx = word.boundsPx.adjusted(-3, -5, 3, 5);
                }
                if (next() < .1) word.inkFitted = true;
                words.push_back(local(word, region.topLeft()));
            }
            // Long lines arrive as overlapping recognition windows.
            const qsizetype span = 9;
            for (qsizetype first = 0; first < words.size(); first += span - 2) {
                Add(c, words.mid(first, span), region, o.source);
                if (first + span >= words.size()) break;
            }
        }
    }
    return c;
}

QVector<Case> GeneratedCases()
{
    QVector<Case> cases{Page("generated-book-383",383,12,0),
        Page("generated-atlas-175",175,7,.06), Page("generated-frame55-262",262,9,-.1),
        Page("generated-high-density-768",768,24,.03)};
    cases.push_back(ReadLike("read-like-dense-3840x2160",{QSize(3840,2160),7,0,2,false,1.6}));
    cases.push_back(ReadLike("read-like-portrait-1654x2339",{QSize(1654,2339),11,0,1,false,1.05}));
    cases.push_back(ReadLike("read-like-dense-1920x1080",{QSize(1920,1080),13,0,1,false,1.1}));
    cases.push_back(ReadLike("read-like-ribbons-1920x1080",{QSize(1920,1080),19,0,2,true,1.1}));
    cases.push_back(ReadLike("read-like-tilted-1920x1080",{QSize(1920,1080),17,.12,1,true}));
    cases.push_back(ReadLike("read-like-rectangles-1920x1080",{QSize(1920,1080),23,0,1,false,1.2,0}));
    cases.push_back(ReadLike("read-like-mixed-shapes-1920x1080",{QSize(1920,1080),29,0,1,true,1.2,.85}));
    Case noOverlap{"no-overlap"};
    for (int i = 0; i < 80; ++i) Add(noOverlap, {Word(QString::number(i),40+(i%8)*400,40+(i/8)*200)});
    cases.push_back(noOverlap);
    Case duplicates{"duplicates-confidence-disposition-and-preferred-row"};
    Add(duplicates,{Word("first",100,100),Word("right",135,100)});
    Add(duplicates,{Word("other-row",100,140)});
    auto weak = Word("weak",100,100,24,12,.7);
    Add(duplicates,{weak,Word("follow-after-rejected",169,100)});
    auto tie = Word("equal-confidence-replaces",100,100);
    Add(duplicates,{tie});
    auto rejected = tie; rejected.disposition=TokenDisposition::kLowConfidence;
    Add(duplicates,{rejected,Word("follow-after-disposition",201,100)});
    Add(duplicates,{Word("spans-rows",90,100,100,60,.99),Word("tail",195,140)});
    cases.push_back(duplicates);
    Case fragments{"fragment-prefix-suffix-and-cross-column"};
    Add(fragments,{Word("recognition",100,100,100,20),Word("column",700,100,100,20)});
    Add(fragments,{Word("rec",90,100,20,20,.91),Word("tion",190,100,20,20,.91)});
    Add(fragments,{Word("recognition",100,100,100,20,.99)});
    Add(fragments,{Word("title",110,97,60,50),Word("code",250,100,30,10)});
    cases.push_back(fragments);
    Case tilted{"rotated-overlap-and-independent-lines"};
    for (double a : {.3,-.3,1.4,-1.4}) {
        Add(tilted,{Word("tilted",140,200,120,16,.9,a),Word("next",270,200,40,16,.9,a)});
        Add(tilted,{Word("winner",140,200,120,16,.95,a)});
    }
    cases.push_back(tilted);
    Case boundaries{"thresholds-and-nextafter"};
    for (double threshold : {.7,.65,.35,.5}) {
        for (double t : {std::nextafter(threshold,0.),threshold,std::nextafter(threshold,1.)}) {
            const double y=double(boundaries.calls.size())*40;
            Add(boundaries,{Word("prefixsuffix",100,y,100,20)});
            Add(boundaries,{Word("prefix",100+100*(1-t),y,100,20,.9)});
        }
    }
    for(double turn : {std::nextafter(.175,0.),.175,std::nextafter(.175,1.)}) {
        Add(boundaries,{Word("angle",200,1000,40,20,.9,turn)});
        Add(boundaries,{Word("near",250,1000,40,20)});
    }
    for(double distance : {std::nextafter(7.,0.),7.,std::nextafter(7.,8.),
                           std::nextafter(80.,0.),80.,std::nextafter(80.,81.)}) {
        Add(boundaries,{Word("size",100,1200+boundaries.calls.size()*30,40,20),
                        Word("gap",140+distance,1200+boundaries.calls.size()*30,40,20)});
    }
    cases.push_back(boundaries);
    Case detail{"detail-edge-margin-translation"};
    for (double x : {std::nextafter(8.,0.),8.,std::nextafter(8.,9.),80.,92.})
        Add(detail,{Word("edge",x,20,8,12)},QRect(100,100,100,100),scene);
    cases.push_back(detail);
    Case invalid{"nan-degenerate-and-overflow-controls"};
    invalid.timed=false;
    const double nan=std::bit_cast<double>(quint64(0x7ff8000000000042));
    for (int i=0;i<7;++i) {
        auto word=Word(QString::number(i),100,i*100,24,12);
        word.quadPx=TextQuad::FromRect(word.boundsPx);
        if(i==0) word.quadPx.points[1].setX(nan);
        if(i==1) word.quadPx.points[2]=word.quadPx.points[1];
        if(i==2) std::swap(word.quadPx.points[1],word.quadPx.points[2]);
        if(i==3) for(auto& p:word.quadPx.points) p*=1e200;
        if(i==4) word.confidence=nan;
        if(i==5) word.boundsPx=QRectF(100,500,0,0);
        if(i==6) word.quadPx.valid=false;
        Add(invalid,{word}); Add(invalid,{word});
    }
    cases.push_back(invalid);
    Case cap{"line-cap-empty-row-reuse",{}, {}, false};
    for(int i=0;i<1024;++i) cap.initial.push_back({{Word(QString::number(i),10,(i+1)*40,8,8)}});
    Add(cap,{Word("at-cap-replaces",10,40,8,8,.99)});
    Add(cap,{Word("cap-refused",3000,3000,8,8)});
    auto fullCap=cap; fullCap.name="line-cap-before-erase-refusal"; cases.push_back(fullCap);
    cap.initial[97].words.clear(); cap.initial[300].words.clear();
    cap.calls.clear();
    Add(cap,{Word("last-empty-row",3500,3500,8,8)});
    cases.push_back(cap);
    Case cacheCap{"cache-cap-fallback",{}, {}, false};
    RecognizedLine many;
    for(int i=0;i<8193;++i) many.words.push_back(Word(QString::number(i),i*32.,100));
    cacheCap.initial.push_back(many);
    Add(cacheCap,{Word("replace-oversize",0,100,24,12,.99)});
    cases.push_back(cacheCap);
    cacheCap.name="cache-cap-transition-within-one-call";
    cacheCap.initial[0].words.removeLast(); cacheCap.calls.clear();
    Add(cacheCap,{Word("new-at-cap",8192*32.,100),Word("then-replace",0,100,24,12,.99)});
    cases.push_back(cacheCap);
    cacheCap.name="cache-8191-seed"; cacheCap.initial[0].words.removeLast(); cases.push_back(cacheCap);

    // A context crossing its word bound in both directions within one
    // sequence: grow past 8192 words, replace at the bound, wipe most words
    // with one wide winner, then grow again from the rebuilt context.
    Case wordBound{"context-word-bound-crossing",{}, {}, false};
    for(int r=0;r<80;++r) {
        RecognizedLine row;
        for(int i=0;i<102;++i) row.words.push_back(Word(QString::number(r*102+i),40+i*30.,40+r*24.,22,12));
        wordBound.initial.push_back(row);
    }
    wordBound.initial.back().words.resize(80); // 8158 words
    for(int k=0;k<40;++k)
        Add(wordBound,{Word("grow"+QString::number(k),3300+(k%4)*60.,40+(k/4)*24.,40,12)});
    Add(wordBound,{Word("replace-at-bound",40,40,22,12,.99)});
    Add(wordBound,{Word("wipe",30,30,3100,1000,.995)});
    for(int k=0;k<12;++k)
        Add(wordBound,{Word("regrow"+QString::number(k),3300+(k%4)*60.,1500+(k/4)*24.,40,12),
                       Word("next"+QString::number(k),3360+(k%4)*60.,1500+(k/4)*24.,40,12)});
    cases.push_back(wordBound);
    // Rows at the 1024 cap: new rows until refusal, then a winner that empties
    // two rows so later words in the same call reuse the last empty one.
    Case rowBound{"context-row-bound-crossing",{}, {}, false};
    for(int i=0;i<1018;++i) rowBound.initial.push_back({{Word(QString::number(i),10+(i%4)*900.,(i/4+1)*15.,8,8)}});
    for(int k=0;k<10;++k) Add(rowBound,{Word("row"+QString::number(k),3800,100+k*300.,8,8)});
    Add(rowBound,{Word("span",5,10,20,30,.99),Word("refused-or-reused",3900,4000,8,8),
                  Word("again",3950,4050,8,8)});
    Add(rowBound,{Word("after-prune",3900,3900,8,8)});
    cases.push_back(rowBound);
    Case oversize{"context-oversize-initial-rows",{}, {}, false};
    for(int i=0;i<1030;++i) oversize.initial.push_back({{Word(QString::number(i),10,(i+1)*3.,8,2)}});
    Add(oversize,{Word("joins",22,3,8,2)}); Add(oversize,{Word("replaces",10,3,8,2,.99)});
    cases.push_back(oversize);

    Case preference{"rejected-later-veto-retains-first-overlap-preference",{}, {}, false};
    preference.initial={{{Word("first-weaker",100,100,24,12,.7)}},
                        {{Word("later-veto",100,104,24,12,.99)}}};
    Add(preference,{Word("rejected-span",100,100,24,20,.9),Word("follower",150,104)});
    cases.push_back(preference);
    Case compaction{"same-call-cross-row-erase-empty-reuse-and-replace",{}, {}, false};
    compaction.initial={{{Word("remove0",100,100),Word("keep1",180,100),
                           Word("remove2",130,100),Word("keep3",230,100)}},
                        {{Word("remove-row",110,130)}}};
    Add(compaction,{Word("wide-winner",95,95,70,60,.99),Word("reuse",700,700),
                    Word("replace-reused",700,700,24,12,.99)});
    cases.push_back(compaction);
    Case ties{"equal-row-distance-and-stable-sort-ties",{}, {}, false};
    ties.initial={{{Word("row0",0,0,20,20)}},{{Word("row1",0,10,20,20)}}};
    Add(ties,{Word("equal-distance",60,5,20,20)});
    Add(ties,{Word("sort-first",200,5,20,20),Word("sort-second",200,5,20,20,.8)});
    cases.push_back(ties);
    Case stable{"equal-baseline-sort-preserves-input-order",{}, {}, false};
    stable.initial={{{Word("first",100,100),Word("second",100,100),Word("third",100,100)}}};
    Add(stable,{}); cases.push_back(stable);

    // Independent predicate controls: unlike the broad overlap workload above,
    // each seed has just one decision, and each call keeps a distinguishable
    // winner. Adjacent doubles and small finite offsets cover rounding near
    // the exact boundaries without assuming subtraction retains every ULP.
    for(double threshold : {.65,.7}) for(double t : {
        threshold-1e-10,std::nextafter(threshold,0.),threshold,
        std::nextafter(threshold,1.),threshold+1e-10}) {
        Case c{"fragment-band-"+QString::number(t,'g',17),{}, {}, false};
        c.initial={{{Word("prefix",0,0,20,20)}}};
        Add(c,{Word("prefix-complete",20*(1-t),0,100,20,.99)}); cases.push_back(c);
    }
    for(double width : {35.-1e-9,std::nextafter(35.,0.),35.,std::nextafter(35.,36.),35.+1e-9}) {
        Case c{"fragment-area-ratio-"+QString::number(width,'g',17),{}, {}, false};
        c.initial={{{Word("suffix",0,0,width,20)}}};
        Add(c,{Word("complete-suffix",width*.34,0,100,20,.99)}); cases.push_back(c);
    }
    for(double h : {10.-1e-9,std::nextafter(10.,0.),10.,std::nextafter(10.,11.),10.+1e-9}) {
        Case c{"row-height-half-"+QString::number(h,'g',17),{}, {}, false};
        c.initial={{{Word("old",0,0,20,20)}}};
        Add(c,{Word("new",40,(20-h)/2,20,h)}); cases.push_back(c);
    }
    for(double d : {7.-1e-9,std::nextafter(7.,0.),7.,std::nextafter(7.,8.),7.+1e-9}) {
        Case c{"row-dy-point35-"+QString::number(d,'g',17),{}, {}, false};
        c.initial={{{Word("old",0,0,20,20)}}};
        Add(c,{Word("new",40,d,20,20)}); cases.push_back(c);
    }
    for(double gap : {80.-1e-9,std::nextafter(80.,0.),80.,std::nextafter(80.,81.),80.+1e-9}) {
        Case c{"row-gap-four-heights-"+QString::number(gap,'g',17),{}, {}, false};
        c.initial={{{Word("old",0,0,20,20)}}};
        Add(c,{Word("new",20+gap,0,20,20)}); cases.push_back(c);
    }
    for(double a : {std::numbers::pi-.01,-std::numbers::pi+.01}) {
        Case c{"row-pi-wrap-"+QString::number(a,'g',17),{}, {}, false};
        c.initial={{{Word("old",100,100,20,20,.9,a)}}};
        Add(c,{Word("new",130,100,20,20,.9,a-std::numbers::pi)}); cases.push_back(c);
    }
    for(int mode=0;mode<6;++mode) {
        Case c{"nan-confidence-distinct-winner-"+QString::number(mode),{}, {}, false};
        auto old=Word("old",100,100); auto incoming=Word("replacement",100,100);
        incoming.inkFitted=true; incoming.paddedBounds=false;
        if(mode%3!=0) old.confidence=nan;
        if(mode%3!=1) incoming.confidence=nan;
        if(mode>=3) incoming.disposition=TokenDisposition::kLowConfidence;
        c.initial={{{old}}}; Add(c,{incoming}); cases.push_back(c);
    }
    for(int side=0;side<4;++side) for(double delta : {-1e-10,0.,1e-10}) {
        Case c{"crop-all-sides-"+QString::number(side)+"-"+QString::number(delta,'g',17),{}, {}, false};
        double x=40,y=40;
        if(side==0)x=8+delta; if(side==1)y=8+delta;
        if(side==2)x=92-8+delta; if(side==3)y=92-12+delta;
        Add(c,{Word("edge",x,y,8,12)},QRect(100,100,100,100),scene); cases.push_back(c);
    }
    Case zeros{"signed-zero-and-invalid-quad-payload-fields",{}, {}, false};
    auto zero=Word("zero",-0.,-0.); zero.confidence=-0.;
    zero.inkSearchPx=QRectF(-0.,-0.,24,12); zero.protectedBoundsPx=zero.inkSearchPx;
    zero.quadPx.points[0]={-0.,nan}; // invalid flag; payload still must survive
    Add(zeros,{zero}); cases.push_back(zeros);
    return cases;
}

QRectF ReadRect(const QJsonValue& value)
{
    const auto a=value.toArray();
    Require(a.size()==4,"Archive rectangle is not four scalars");
    return {a[0].toDouble(),a[1].toDouble(),a[2].toDouble(),a[3].toDouble()};
}
void AddArchive(QVector<Case>& cases, const QString& path)
{
    QFile manifest(path); Require(manifest.open(QIODevice::ReadOnly),"Cannot open archive manifest");
    const auto entries=QJsonDocument::fromJson(manifest.readAll()).object().value("receipts").toArray();
    Require(!entries.isEmpty() && entries.size()<=8,"Archive needs one to eight receipts");
    for(const auto& entry:entries) {
        const auto object=entry.toObject(); QFile file(object.value("path").toString());
        Require(file.open(QIODevice::ReadOnly) && file.size()<=16*1024*1024,"Cannot open bounded archived receipt");
        const auto bytes=file.readAll();
        Require(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()==object.value("sha256").toString().toLatin1(),"Archive hash changed");
        const auto receipt=QJsonDocument::fromJson(bytes).object();
        const auto lines=receipt.value("result").toObject().value("output").toObject().value("lines").toArray();
        Require(!lines.isEmpty() && lines.size()<=1024,"Archive output lines absent/oversized");
        Case c{"archive-final-output-regrouped-"+object.value("name").toString()};
        const QSize size(object.value("width").toInt(),object.value("height").toInt());
        Require(!size.isEmpty(),"Archive source dimensions absent");
        int count=0;
        for(const auto& line:lines) {
            QVector<RecognizedWord> words;
            for(const auto& value:line.toArray()) {
                Require(++count<=8192,"Archive word bound exceeded");
                const auto w=value.toObject(); RecognizedWord word;
                word.text=w.value("text").toString(); word.boundsPx=ReadRect(w.value("bounds"));
                word.inkSearchPx=ReadRect(w.value("inkSearch"));
                word.protectedBoundsPx=ReadRect(w.value("protectedBounds"));
                word.confidence=w.value("confidence").toDouble();
                word.disposition=TokenDisposition(w.value("disposition").toInt());
                word.paddedBounds=w.value("paddedBounds").toBool();
                word.colors=LineColors{QColor(w.value("foreground").toString()),QColor(w.value("background").toString())};
                // Historical receipts omit quads/inkFitted and pre-merge crop
                // order. Default those missing fields; this is explicitly an
                // output-derived microbenchmark, never a true call-trace replay.
                words.push_back(word);
            }
            Add(c,words,QRect(QPoint(),size),size);
        }
        const auto original=c.calls;
        for(const auto& call:original) c.calls.push_back(call);
        cases.push_back(c);
    }
}
void GeometryControls()
{
    QVector<TextQuad> quads;
    for(double a : {0.,.001,.175,.4,-.4,1.57})
        for(double x : {0.,19.999999999,20.,40.})
            quads.push_back(Word("q",x,2,20,10,.9,a).quadPx.valid ?
                Word("q",x,2,20,10,.9,a).quadPx : TextQuad::FromRect(QRectF(x,2,20,10)));
    auto reverse=quads[0]; std::reverse(reverse.points.begin(),reverse.points.end()); quads.push_back(reverse);
    auto invalid=quads[0]; invalid.points[1]=invalid.points[0]; quads.push_back(invalid);
    invalid=quads[0]; std::swap(invalid.points[1],invalid.points[2]); quads.push_back(invalid);
    invalid=quads[0]; invalid.points[0].setX(std::numeric_limits<double>::quiet_NaN()); quads.push_back(invalid);
    invalid=quads[0]; invalid.points[0].setY(std::numeric_limits<double>::infinity()); quads.push_back(invalid);
    invalid=quads[0]; invalid.points[2]={1,3}; quads.push_back(invalid); // concave
    quads.push_back(TextQuad::FromRect(QRectF(0,0,std::ldexp(1.,-30),std::ldexp(1.,-30))));
    quads.push_back(TextQuad::FromRect(QRectF(std::ldexp(1.,53),std::ldexp(1.,53),2,2)));
    quads.push_back(TextQuad::FromRect(QRectF(std::ldexp(1.,600),std::ldexp(1.,600),std::ldexp(1.,600),std::ldexp(1.,600))));
    for(double magnitude : {1e150,1e200,1e300}) {
        auto huge=quads[0]; for(auto& p:huge.points) p*=magnitude; quads.push_back(huge);
    }
    for(const auto& a:quads) {
        Require(std::bit_cast<quint64>(QuadArea(a))==std::bit_cast<quint64>(merge_replay::FrozenQuadArea(a)),"Quad area bit mismatch");
        for(const auto& b:quads)
            Require(std::bit_cast<quint64>(QuadIntersectionArea(a,b))==std::bit_cast<quint64>(merge_replay::FrozenQuadIntersectionArea(a,b)),"Quad intersection bit mismatch");
    }
}
enum class Arm { Bb53, Ec72, Context, Free };
QString ArmName(Arm arm)
{
    switch(arm) {
    case Arm::Bb53: return "frozen-bb53";
    case Arm::Ec72: return "baseline";
    case Arm::Context: return "candidate";
    case Arm::Free: return "candidate-without-context";
    }
    return {};
}
// One merge function per visit; the candidate keeps one context for the
// whole visit, exactly as a recognizer keeps one per read.
class Merger {
  public:
    explicit Merger(Arm arm) : arm_(arm) {}
    void operator()(Lines& lines, const Call& call)
    {
        switch(arm_) {
        case Arm::Bb53: merge_replay::FrozenPaddleMergeRecognition(lines,call.line,call.region,call.source); break;
        case Arm::Ec72: merge_replay::FrozenEc72PaddleMergeRecognition(lines,call.line,call.region,call.source); break;
        case Arm::Context: PaddleMergeRecognition(lines,call.line,call.region,call.source,context_); break;
        case Arm::Free: PaddleMergeRecognition(lines,call.line,call.region,call.source); break;
        }
    }
  private:
    Arm arm_;
    PaddleMergeContext context_;
};
QVector<QByteArray> Expected(const Case& c, Arm arm = Arm::Ec72)
{
    Lines lines=c.initial; QVector<QByteArray> expected; Merger merge(arm);
    for(const auto& call:c.calls) {
        merge(lines,call);
        expected.push_back(Snapshot(lines));
    }
    return expected;
}
// bb53 predates refused-ribbon protection inheritance (73e3e4d). Without an
// inheritable input word no inheritance can happen, and ec7282d must equal it.
bool InheritableInput(const Case& c)
{
    const auto inheritable=[](const RecognizedWord& w) {
        return !w.protectedBoundsPx.isEmpty() && w.disposition==TokenDisposition::kInvalidGeometry &&
               (w.fromRibbon || w.ribbonErrorPx<0);
    };
    for(const auto& row:c.initial) for(const auto& w:row.words) if(inheritable(w)) return true;
    for(const auto& call:c.calls) for(const auto& w:call.line.words) if(inheritable(w)) return true;
    return false;
}
QByteArray FrozenInputs(const QVector<Case>& cases)
{
    QByteArray bytes;
    QDataStream out(&bytes,QIODevice::WriteOnly); out.setVersion(QDataStream::Qt_6_0);
    out << kFormat << quint64(cases.size());
    for(const auto& c:cases) {
        out << c.name << Snapshot(c.initial) << c.timed << quint64(c.calls.size());
        for(const auto& call:c.calls)
            out << Snapshot(Lines{call.line}) << call.region << call.source;
    }
    return bytes;
}
QVector<Case> LoadFrozen(const QString& path)
{
    QFile file(path); Require(file.open(QIODevice::ReadOnly) && file.size()<=64*1024*1024,"Cannot open bounded frozen corpus");
    QDataStream in(file.readAll()); in.setVersion(QDataStream::Qt_6_0);
    quint32 version{}; quint64 count{}; in >> version >> count;
    Require((version==1 || version==kFormat) && count<=128,"Unsupported frozen corpus"); QVector<Case> cases;
    for(quint64 i=0;i<count;++i) {
        Case c; QByteArray initial; quint64 calls{};
        in >> c.name >> initial >> c.timed >> calls; c.initial=Restore(initial,version);
        Require(calls<=10000,"Frozen call bound exceeded");
        for(quint64 j=0;j<calls;++j) {
            QByteArray line; Call call; in >> line >> call.region >> call.source;
            const auto rows=Restore(line,version); Require(rows.size()==1,"Frozen call needs one input line");
            call.line=rows[0]; c.calls.push_back(call);
        }
        cases.push_back(c);
    }
    Require(in.status()==QDataStream::Ok && in.atEnd(),"Frozen corpus truncated/trailing");
    return cases;
}
void SaveBytes(const QString& path,const QByteArray& bytes)
{
    QSaveFile file(path); Require(file.open(QIODevice::WriteOnly),"Cannot create frozen input artifact");
    Require(file.write(bytes)==bytes.size() && file.commit(),"Cannot commit frozen inputs");
}
QJsonObject Visit(const Case& c, const QVector<QByteArray>& expected, Arm arm,
                  int block, int position, QElapsedTimer& deadline)
{
    Lines lines=c.initial; QJsonArray times; qint64 total=0; Merger merge(arm);
    for(qsizetype i=0;i<c.calls.size();++i) {
        Require(deadline.elapsed()<240000,"CPU replay exceeded four-minute execution budget");
        const auto& call=c.calls[i]; QElapsedTimer timer; timer.start();
        merge(lines,call);
        const auto ns=timer.nsecsElapsed(); total+=ns; times.push_back(double(ns));
        Require(Snapshot(lines)==expected[i],c.name+" "+ArmName(arm)+" bit mismatch after call "+QString::number(i));
    }
    return {{"case",c.name},{"arm",ArmName(arm)},{"block",block},{"position",position},
            {"mergeNanoseconds",double(total)},{"callNanoseconds",times},{"correctAfterEveryCall",true}};
}
// A context must never let stale cache decide: one context alternating
// between two outputs, a different source size, a moved context, and outside
// edits between calls (geometry moved, words swapped, erased, appended,
// reversed, rows cleared, quads dropped, text/confidence changed) must all
// give the oracle's result on the same edited output.
void ContextControls()
{
    const auto base=GeneratedCases().front().calls; // generated-book-383
    Require(base.size()>40,"Context control needs a long call sequence");
    const auto edit=[](Lines& lines, int kind) {
        if(lines.isEmpty()) return;
        // Taken only by the edits that write, so the read-only and copy
        // kinds leave the output's buffer untouched.
        const auto row=[&]() -> QVector<RecognizedWord>& { return lines[(kind*7)%lines.size()].words; };
        switch(kind%12) {
        case 0: if(!row().isEmpty()) { auto& w=row()[0]; w.boundsPx.translate(3,1);
                    if(w.quadPx.valid) w.quadPx=w.quadPx.Translated({3,1}); } break;
        case 1: if(row().size()>1) std::swap(row()[0],row()[row().size()-1]); break;
        case 2: if(!row().isEmpty()) row().removeAt(row().size()/2); break;
        case 3: std::reverse(row().begin(),row().end()); break;
        case 4: lines.push_back({{Word("outside",3000,3000)}}); break;
        case 5: row().clear(); break; // an empty row left for the merge to reuse
        case 6: for(auto& w:row()) { w.text+="x"; w.confidence=.999; w.disposition=TokenDisposition::kCandidate; } break;
        case 7: if(!row().isEmpty()) { row()[0].quadPx=TextQuad::FromRect(row()[0].boundsPx).Translated({0,.5}); } break;
        case 8: lines.clear(); break;
        // A copy edited elsewhere leaves the output's buffer alone: reuse.
        case 9: { auto copy=lines; copy[0].words.clear(); copy.push_back({}); } break;
        // The recognizer's final row sort, applied between calls.
        case 10: std::sort(lines.begin(),lines.end(),[](const auto& a,const auto& b) {
                     return !a.words.isEmpty() && !b.words.isEmpty() &&
                            a.words[0].boundsPx.top()>b.words[0].boundsPx.top(); }); break;
        // Read-only access: reuse.
        case 11: Require(std::as_const(lines).constFirst().words.size()>=0,"read"); break;
        }
    };
    // Outside edits every few calls, applied identically to both outputs.
    for(int every : {1,3,11}) {
        Lines want, got; PaddleMergeContext context; int kind=every;
        for(qsizetype i=0;i<base.size();++i) {
            if(i%every==every-1) { edit(want,kind); edit(got,kind); ++kind; }
            merge_replay::FrozenEc72PaddleMergeRecognition(want,base[i].line,base[i].region,base[i].source);
            PaddleMergeRecognition(got,base[i].line,base[i].region,base[i].source,context);
            Require(Snapshot(got)==Snapshot(want),"Context outside edit mismatch at call "+QString::number(i)+
                    " every "+QString::number(every));
        }
    }
    // One context alternating between two outputs, then moved, then given a
    // different source size for the same output.
    Lines wantA, wantB, gotA, gotB; PaddleMergeContext shared;
    for(qsizetype i=0;i<base.size();++i) {
        auto& want=i%2 ? wantB : wantA; auto& got=i%2 ? gotB : gotA;
        merge_replay::FrozenEc72PaddleMergeRecognition(want,base[i].line,base[i].region,base[i].source);
        if(i==base.size()/2) { PaddleMergeContext moved(std::move(shared)); shared=std::move(moved); }
        PaddleMergeRecognition(got,base[i].line,base[i].region,base[i].source,shared);
        Require(Snapshot(got)==Snapshot(want),"Shared context mismatch at call "+QString::number(i));
    }
    Lines want=wantA, got=gotA; PaddleMergeContext context;
    for(int size : {4096,2048,4096,640}) {
        const QSize source(size,size);
        auto line=base.front().line;
        merge_replay::FrozenEc72PaddleMergeRecognition(want,line,QRect(QPoint(),source),source);
        PaddleMergeRecognition(got,line,QRect(QPoint(),source),source,context);
        Require(Snapshot(got)==Snapshot(want),"Context source change mismatch at "+QString::number(size));
    }
}
void Save(const QString& path, const QJsonObject& object)
{
    QSaveFile file(path); Require(file.open(QIODevice::WriteOnly),"Cannot create report");
    const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Indented);
    Require(file.write(bytes)==bytes.size() && file.commit(),"Cannot commit report");
}
} // namespace
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv); QJsonArray raw; QString output;
    try {
        const auto args=app.arguments(); bool benchmark=false; QString archive,frozenPath,only;
        for(int i=1;i<args.size();++i) {
            if(args[i]=="--benchmark") benchmark=true;
            else if(args[i]=="--output" && i+1<args.size()) output=args[++i];
            else if(args[i]=="--archive-manifest" && i+1<args.size()) archive=args[++i];
            else if(args[i]=="--frozen-input" && i+1<args.size()) frozenPath=args[++i];
            else if(args[i]=="--only" && i+1<args.size()) only=args[++i];
            else throw std::runtime_error("Unknown/incomplete argument");
        }
        Require(!benchmark || !output.isEmpty(),"Benchmark requires an explicit output path");
        QElapsedTimer deadline; deadline.start(); GeometryControls();
        Require(frozenPath.isEmpty() || archive.isEmpty(),"Select frozen inputs or archive regrouping");
        auto cases=frozenPath.isEmpty() ? GeneratedCases() : LoadFrozen(frozenPath);
        if(!archive.isEmpty()) AddArchive(cases,archive);
        if(frozenPath.isEmpty() && only.isEmpty()) ContextControls();
        // Development filter: keep only cases whose name contains this text.
        if(!only.isEmpty()) cases.removeIf([&](const Case& c) { return !c.name.contains(only); });
        const auto frozen=FrozenInputs(cases);
        if(!frozenPath.isEmpty()) {
            QFile original(frozenPath);
            Require(original.open(QIODevice::ReadOnly) && original.readAll()==frozen,
                    "Frozen input roundtrip changed serialized field bits");
        }
        if(!output.isEmpty()) SaveBytes(output+".inputs.bin",frozen);
        const auto inputHash=QCryptographicHash::hash(frozen,QCryptographicHash::Sha256).toHex();
        QJsonArray coverage;
        for(const auto& c:cases) {
            const auto expected=Expected(c);
            const bool inheritable=InheritableInput(c);
            if(!inheritable) Require(Expected(c,Arm::Bb53)==expected,c.name+": ec7282d oracle differs from bb53");
            QCryptographicHash digest(QCryptographicHash::Sha256);
            for(const auto& bytes:expected) digest.addData(bytes);
            const auto final=expected.isEmpty() ? Snapshot(c.initial) : expected.last();
            const auto finalLines=Restore(final);
            qsizetype finalWords=0; for(const auto& row:finalLines) finalWords+=row.words.size();
            coverage.push_back(QJsonObject{{"case",c.name},{"calls",double(c.calls.size())},
                {"finalRows",double(finalLines.size())},{"finalWords",double(finalWords)},
                {"bb53Compared",!inheritable},
                {"allFieldsExpectedSha256",QString::fromLatin1(digest.result().toHex())}});
            std::printf("%s: %lld calls, %lld rows, %lld words\n",qPrintable(c.name),
                        qlonglong(c.calls.size()),qlonglong(finalLines.size()),qlonglong(finalWords));
            raw.push_back(Visit(c,expected,Arm::Context,-2,0,deadline));
            raw.push_back(Visit(c,expected,Arm::Free,-3,0,deadline));
            if(!benchmark || !c.timed) continue;
            for(int warm=0;warm<5;++warm) {
                raw.push_back(Visit(c,expected,Arm::Ec72,-1,warm,deadline));
                raw.push_back(Visit(c,expected,Arm::Context,-1,warm,deadline));
            }
            for(int block=0;block<31;++block) for(int pos=0;pos<4;++pos) {
                const bool baseline=(block%2==0 ? (pos==0 || pos==3) : (pos==1 || pos==2));
                raw.push_back(Visit(c,expected,baseline ? Arm::Ec72 : Arm::Context,block,pos,deadline));
            }
        }
        QJsonObject report{{"complete",true},{"baselineCommit","ec7282db3a79fc760e1ff3b714807115f06cc5bf"},
            {"correctnessOracles","ec7282d for every case; bb53 also where no input carries inheritable protection"},
            {"scope","CPU merge function microtime; generated and optional regrouped final-output inputs; not a GPU/worker/FPS measurement"},
            {"warmupsPerArm",benchmark?5:0},{"pairedBlocks",benchmark?31:0},
            {"blockOrder","ABBA, BAAB alternating"},{"inputFormat",double(kFormat)},{"coverage",coverage},{"rawVisits",raw}};
        report.insert("frozenInputsSha256",QString::fromLatin1(inputHash));
        report.insert("frozenInputBytes",frozen.size());
        if(!output.isEmpty()) Save(output,report);
        std::puts("PASS: frozen geometry and all-field merge equivalence after every call");
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"FAIL: %s\n",e.what());
        if(!output.isEmpty()) {
            try { Save(output,{{"complete",false},{"error",QString::fromUtf8(e.what())},{"rawVisits",raw}}); }
            catch(...) { std::fputs("Could not preserve partial report\n",stderr); }
        }
        return 1;
    }
}
