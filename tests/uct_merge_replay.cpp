#include "uct_merge_baseline_bb53.hpp"

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
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace okuflow::uct;
namespace {
using Lines = QVector<RecognizedLine>;
using Merge = void (*)(Lines&, RecognizedLine, QRect, QSize);
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
QByteArray Snapshot(const Lines& lines)
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
Lines Restore(const QByteArray& bytes)
{
    QDataStream in(bytes); in.setVersion(QDataStream::Qt_6_0);
    quint64 rows{}; in >> rows; Require(rows<=1024,"Frozen row bound exceeded");
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
QVector<Case> GeneratedCases()
{
    QVector<Case> cases{Page("generated-book-383",383,12,0),
        Page("generated-atlas-175",175,7,.06), Page("generated-frame55-262",262,9,-.1),
        Page("generated-high-density-768",768,24,.03)};
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
QVector<QByteArray> Expected(const Case& c)
{
    Lines lines=c.initial; QVector<QByteArray> expected;
    for(const auto& call:c.calls) {
        merge_replay::FrozenPaddleMergeRecognition(lines,call.line,call.region,call.source);
        expected.push_back(Snapshot(lines));
    }
    return expected;
}
QByteArray FrozenInputs(const QVector<Case>& cases)
{
    QByteArray bytes;
    QDataStream out(&bytes,QIODevice::WriteOnly); out.setVersion(QDataStream::Qt_6_0);
    out << quint32(1) << quint64(cases.size());
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
    Require(version==1 && count<=128,"Unsupported frozen corpus"); QVector<Case> cases;
    for(quint64 i=0;i<count;++i) {
        Case c; QByteArray initial; quint64 calls{};
        in >> c.name >> initial >> c.timed >> calls; c.initial=Restore(initial);
        Require(calls<=10000,"Frozen call bound exceeded");
        for(quint64 j=0;j<calls;++j) {
            QByteArray line; Call call; in >> line >> call.region >> call.source;
            const auto rows=Restore(line); Require(rows.size()==1,"Frozen call needs one input line");
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
QJsonObject Visit(const Case& c, const QVector<QByteArray>& expected, Merge merge,
                  const QString& arm, int block, int position, QElapsedTimer& deadline)
{
    Lines lines=c.initial; QJsonArray times; qint64 total=0;
    for(qsizetype i=0;i<c.calls.size();++i) {
        Require(deadline.elapsed()<240000,"CPU replay exceeded four-minute execution budget");
        const auto& call=c.calls[i]; QElapsedTimer timer; timer.start();
        merge(lines,call.line,call.region,call.source);
        const auto ns=timer.nsecsElapsed(); total+=ns; times.push_back(double(ns));
        Require(Snapshot(lines)==expected[i],c.name+" "+arm+" bit mismatch after call "+QString::number(i));
    }
    return {{"case",c.name},{"arm",arm},{"block",block},{"position",position},
            {"mergeNanoseconds",double(total)},{"callNanoseconds",times},{"correctAfterEveryCall",true}};
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
        const auto args=app.arguments(); bool benchmark=false; QString archive,frozenPath;
        for(int i=1;i<args.size();++i) {
            if(args[i]=="--benchmark") benchmark=true;
            else if(args[i]=="--output" && i+1<args.size()) output=args[++i];
            else if(args[i]=="--archive-manifest" && i+1<args.size()) archive=args[++i];
            else if(args[i]=="--frozen-input" && i+1<args.size()) frozenPath=args[++i];
            else throw std::runtime_error("Unknown/incomplete argument");
        }
        Require(!benchmark || !output.isEmpty(),"Benchmark requires an explicit output path");
        QElapsedTimer deadline; deadline.start(); GeometryControls();
        Require(frozenPath.isEmpty() || archive.isEmpty(),"Select frozen inputs or archive regrouping");
        auto cases=frozenPath.isEmpty() ? GeneratedCases() : LoadFrozen(frozenPath);
        if(!archive.isEmpty()) AddArchive(cases,archive);
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
            QCryptographicHash digest(QCryptographicHash::Sha256);
            for(const auto& bytes:expected) digest.addData(bytes);
            coverage.push_back(QJsonObject{{"case",c.name},{"calls",double(c.calls.size())},
                {"allFieldsExpectedSha256",QString::fromLatin1(digest.result().toHex())}});
            raw.push_back(Visit(c,expected,PaddleMergeRecognition,"candidate",-2,0,deadline));
            if(!benchmark || !c.timed) continue;
            for(int warm=0;warm<5;++warm) {
                raw.push_back(Visit(c,expected,merge_replay::FrozenPaddleMergeRecognition,"baseline",-1,warm,deadline));
                raw.push_back(Visit(c,expected,PaddleMergeRecognition,"candidate",-1,warm,deadline));
            }
            for(int block=0;block<31;++block) for(int pos=0;pos<4;++pos) {
                const bool baseline=(block%2==0 ? (pos==0 || pos==3) : (pos==1 || pos==2));
                raw.push_back(Visit(c,expected,baseline ? merge_replay::FrozenPaddleMergeRecognition : PaddleMergeRecognition,
                    baseline ? "baseline" : "candidate",block,pos,deadline));
            }
        }
        QJsonObject report{{"complete",true},{"baselineCommit","bb53ac884c24d15f0c77b2dc43cace0e8e650c24"},
            {"scope","CPU merge function microtime; generated and optional regrouped final-output inputs; not a GPU/worker/FPS measurement"},
            {"warmupsPerArm",benchmark?5:0},{"pairedBlocks",benchmark?31:0},
            {"blockOrder","ABBA, BAAB alternating"},{"coverage",coverage},{"rawVisits",raw}};
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
