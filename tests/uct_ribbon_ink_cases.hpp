#pragma once
#include "okuflow/common/uct_current_ink.hpp"
#include "okuflow/common/uct_paddle.hpp"
#include "ultra_clear_text_test_support.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/uct_screen_text_accessible.hpp"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QDir>
#include <QLineF>
#include <QTransform>
#include <QtTest>
#include <cstring>
#include <limits>

namespace uct_ribbon_test {
using namespace okuflow::uct;
inline SourceStamp Stamp(QSize size) {
    SourceStamp s;s.sourceEpoch=1;s.geometryGeneration=4;s.frameSeq=42;s.timestampMs=100;
    s.viewportWidth=size.width();s.viewportHeight=size.height();s.viewTransform={0,0,1,1,0,0,1,1,true};
    s.immutableSelectedInput=true;return s;
}
inline PaddleQuad Analytic(double curvature=40) {
    PaddleQuad q{{QPointF(32,40),QPointF(288,40),QPointF(288,64),QPointF(32,64)},.98f};
    q.ribbon={32,64,256,0,0,1,0,-curvature,curvature,12,0,0,1,true};return q;
}
inline std::vector<float> DetectorMap(bool curve) {
    std::vector<float> map(320*160);
    for(int y=0;y<160;++y)for(int x=32;x<=288;++x) {
        const double center=60+(curve?.0006*(x-160)*(x-160):0);
        if(std::abs(y-center)<=8)map[y*320+x]=.95f;
    }return map;
}
inline QImage Gradient() {
    QImage image(384,160,QImage::Format_ARGB32);
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)
        image.setPixel(x,y,qRgb(x/2,y,(x/2+y)/2));
    return image;
}
inline void DetectionAndSampling() {
    std::atomic<bool> cancelled{};
    const auto straight=DetectorMap(false);
    const auto control=PaddleDetect(straight,320,160,{320,160},cancelled);
    const auto enabled=PaddleDetect(straight,320,160,{320,160},cancelled,true);
    QCOMPARE(control.size(),1);QCOMPARE(enabled.size(),1);QVERIFY(!enabled[0].ribbon.valid);
    QCOMPARE(control[0].points,enabled[0].points);QCOMPARE(control[0].score,enabled[0].score);
    const auto source=Gradient();
    const auto a=PaddleRectify(source,control[0],1024,410),b=PaddleRectify(source,enabled[0],1024,410);
    QCOMPARE(a.size(),b.size());QVERIFY(std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0);
    const auto wa=PaddleRecognitionWindows(control[0]),wb=PaddleRecognitionWindows(enabled[0]);
    QCOMPARE(wa.size(),wb.size());for(int i=0;i<wa.size();++i)QCOMPARE(wa[i].points,wb[i].points);
    const QStringList dictionary{"","read"," ","ink"};
    const std::array<PaddleToken,3> tokens{{{1,3,10,.99f},{2,12,13,.99f},{3,18,23,.99f}}};
    const auto da=PaddleDecodeTokens(tokens,32,control[0],dictionary),db=PaddleDecodeTokens(tokens,32,enabled[0],dictionary);
    QCOMPARE(da.words.size(),db.words.size());
    for(int i=0;i<da.words.size();++i) {
        QCOMPARE(da.words[i].text,db.words[i].text);QCOMPARE(da.words[i].boundsPx,db.words[i].boundsPx);
        QCOMPARE(da.words[i].quadPx.points,db.words[i].quadPx.points);QVERIFY(!db.words[i].fromRibbon);
    }
    const auto curves=PaddleDetect(DetectorMap(true),320,160,{320,160},cancelled,true);
    QCOMPARE(curves.size(),1);QVERIFY(curves[0].ribbon.valid);
    for(int i=0;i<=32;++i) {
        const auto p=curves[0].ribbon.At(i/32.,.5);
        QVERIFY(std::abs(p.y-(60+.0006*(p.x-160)*(p.x-160)))<.8);
    }
    const auto q=Analytic();const int valid=256,canvas=512;
    const auto crop=PaddleRectify(source,q,canvas,valid);QCOMPARE(crop.size(),std::size_t(canvas)*48*3);
    // Analytic source red channel is linear between native pixels. This oracle
    // does not call production sampling or rectify a pre-flattened image.
    for(int y=0;y<48;++y)for(int x=0;x<valid;++x) {
        const double u=(x+.5)/valid,v=(y+.5)/48;
        const double px=32+256*u,py=64-40*u+40*u*u+(2*v-1)*12;
        const int ix=int(px-.5);const double fraction=px-.5-ix;
        const double expected=((ix/2)*(1-fraction)+((ix+1)/2)*fraction)/255*2-1;
        QVERIFY(std::abs(crop[2*canvas*48+y*canvas+x]-expected)<2e-6);
        QVERIFY(std::abs(crop[canvas*48+y*canvas+x]-((py-.5)/255*2-1))<2e-6);
    }
    for(int y=0;y<48;++y)for(int x=valid;x<canvas;++x)QCOMPARE(crop[y*canvas+x],0.f);
    const auto raw=PaddleRectify(source,q,canvas,valid);
    auto rectangular=q;rectangular.ribbon={};
    const auto wrong=PaddleRectify(source,rectangular,canvas,valid);QVERIFY(raw!=wrong);
}
inline void BoundsAndWindows() {
    std::atomic<bool> cancelled{};
    std::vector<float> tiny(32*32);
    for(int y=4;y<7;++y)for(int x=4;x<7;++x)tiny[y*32+x]=.9f;
    const auto minimum=PaddleDetect(tiny,32,32,{32,32},cancelled,true);
    QCOMPARE(minimum.size(),1);QVERIFY(!minimum[0].ribbon.valid);
    tiny[4*32+4]=0;QVERIFY(PaddleDetect(tiny,32,32,{32,32},cancelled,true).isEmpty());
    std::fill(tiny.begin(),tiny.end(),0);for(int x=0;x<32;++x)tiny[16*32+x]=.9f;
    QVERIFY(PaddleDetect(tiny,32,32,{32,32},cancelled,true).isEmpty());
    QVERIFY(PaddleDetect({},4097,1,{4097,1},cancelled,true).isEmpty());
    QVERIFY(PaddleDetect({},0,0,{},cancelled,true).isEmpty());
    cancelled=true;QVERIFY(PaddleDetect(DetectorMap(true),320,160,{320,160},cancelled,true).isEmpty());cancelled=false;
    std::vector<float> large(600*600,.9f);
    const auto full=PaddleDetect(large,600,600,{600,600},cancelled,true);
    QCOMPARE(full.size(),1);QVERIFY(!full[0].ribbon.valid);
    ribbon::Bin bins[ribbon::kBins];
    for(int i=0;i<ribbon::kBins;++i) {
        const double t=(i+.5)/ribbon::kBins,center=80-80*t+80*t*t;
        ribbon::Add(bins,t,center-5);ribbon::Add(bins,t,center+5);
    }
    QVERIFY(ribbon::Fit(bins,20,420,1,0,1,1,500,200).valid);
    const auto refusedFit=ribbon::Fit(bins,20,420,1,0,1,1,500,40);
    QVERIFY(!refusedFit.valid);QVERIFY(refusedFit.unsupported);
    auto refusedLine=Analytic();refusedLine.ribbon=refusedFit;
    const QStringList dictionary{"","read"};const std::array<PaddleToken,1> one{{{1,3,12,.99f}}};
    const auto refusedWords=PaddleDecodeTokens(one,32,refusedLine,dictionary);
    QCOMPARE(refusedWords.words.size(),1);QCOMPARE(refusedWords.words[0].disposition,TokenDisposition::kInvalidGeometry);
    QCOMPARE(refusedWords.words[0].ribbonErrorPx,-1.);
    refusedLine.points={QPointF(10,20),QPointF(4010,20),QPointF(4010,80),QPointF(10,80)};
    refusedLine.ribbon.protectLeft=10;refusedLine.ribbon.protectTop=20;
    refusedLine.ribbon.protectRight=4010;refusedLine.ribbon.protectBottom=80;
    const std::array<PaddleToken,1> middle{{{1,10,18,.99f}}};
    const auto cutWindows=PaddleRecognitionWindows(refusedLine);QVERIFY(cutWindows.size()>1);
    for(const auto& window:cutWindows) {
        const auto words=PaddleDecodeTokens(middle,32,window,dictionary);QVERIFY(!words.words.isEmpty());
        for(const auto& word:words.words)QCOMPARE(word.protectedBoundsPx,QRectF(10,20,4000,60));
    }

    QVERIFY(!ribbon::Fit(bins,20,20,1,0,1,1,500,200).valid);
    QVERIFY(!ribbon::Fit(bins,20,420,1,0,-1,1,500,200).valid);
    bins[4]={};bins[5]={};bins[6]={};bins[7]={};bins[8]={};
    QVERIFY(!ribbon::Fit(bins,20,420,1,0,1,1,500,200).valid);
    auto q=Analytic();q.ribbon.ux=30000;
    const auto windows=PaddleRecognitionWindows(q);QVERIFY(windows.size()<=9);QVERIFY(windows.size()>1);
    double lastEnd=0;
    for(int i=1;i<windows.size();++i) {
        QVERIFY(windows[i].ribbon.valid);QVERIFY(windows[i].ribbon.start<=lastEnd+1e-9);
        lastEnd=windows[i].ribbon.end;
    }QVERIFY(std::abs(lastEnd-1)<1e-8);
    auto stamp=Stamp({320,160});QVERIFY(PaddleRibbonInputAllowed(stamp));
    stamp.immutableSelectedInput=false;QVERIFY(!PaddleRibbonInputAllowed(stamp));
    stamp.immutableSelectedInput=true;stamp.temporallySmoothed=true;QVERIFY(!PaddleRibbonInputAllowed(stamp));
    stamp.temporallySmoothed=false;stamp.liveCameraCapture=true;QVERIFY(!PaddleRibbonInputAllowed(stamp));
    stamp.liveCameraCapture=false;stamp.holdDisplay=HoldDisplay::Held;QVERIFY(!PaddleRibbonInputAllowed(stamp));
    QVERIFY(PaddleRibbonEnabled(true, {}));
    QVERIFY(PaddleRibbonEnabled(true, "1"));
    QVERIFY(!PaddleRibbonEnabled(true, "0"));
    QVERIFY(!PaddleRibbonEnabled(true, "invalid"));
    QVERIFY(!PaddleRibbonEnabled(false, {}));
    QVERIFY(!PaddleRibbonEnabled(false, "1"));
}
inline RecognizedLine Words(const PaddleQuad& q,bool longWord=false) {
    const QStringList dictionary{"","READ"," ","ink"};
    const std::array<PaddleToken,3> shortTokens{{{1,10,15,.99f},{2,20,20,.99f},{3,45,50,.99f}}};
    const std::array<PaddleToken,1> longTokens{{{1,3,90,.99f}}};
    return longWord?PaddleDecodeTokens(longTokens,100,q,dictionary):PaddleDecodeTokens(shortTokens,100,q,dictionary);
}
inline void WordsAndComposition() {
    const auto q=Analytic(80);
    const auto accepted=Words(q);QCOMPARE(accepted.words.size(),2);
    for(const auto& word:accepted.words) {QVERIFY(word.fromRibbon);QCOMPARE(word.disposition,TokenDisposition::kCandidate);QVERIFY(word.ribbonErrorPx<1);}
    const auto refused=Words(q,true);QCOMPARE(refused.words.size(),1);
    QCOMPARE(refused.words[0].disposition,TokenDisposition::kInvalidGeometry);QVERIFY(refused.words[0].ribbonErrorPx>1);
    QVERIFY(refused.words[0].protectedBoundsPx.contains(refused.words[0].boundsPx));
    const double expectedSagitta=80*std::pow((90+1.5-2.5)/100,2)*.25;
    QVERIFY(std::abs(refused.words[0].ribbonErrorPx-expectedSagitta)<1e-9);
    RecognitionOutput out;out.ok=true;out.lines={refused};FinalizeRecognizedGeometry(out);
    QCOMPARE(out.lines[0].words[0].disposition,TokenDisposition::kInvalidGeometry);
    // Existing processing has already applied H (rotation + crop + keystone).
    // The backward ribbon samples that analysis image. Test its actual CTC,
    // crop merge, scene conversion and present reprojection composition.
    QTransform rotation;rotation.rotate(17);rotation.translate(-23,-11);
    QTransform keystone(1.07,.03,.0003,-.04,.97,-.00015,31,22,1);
    const auto H=rotation*keystone;bool inverseOk=false;const auto inverse=H.inverted(&inverseOk);QVERIFY(inverseOk);
    QVector<RecognizedLine> merged;const QRect region(40,24,320,160);
    PaddleMergeRecognition(merged,accepted,region,{480,260});QVERIFY(!merged.isEmpty());
    RecognitionResult result;result.stamp=Stamp({480,260});
    result.stamp.viewTransform={.12f,.08f,.68f,.76f,.03f,.06f,.9f,.86f,true};result.output.ok=true;result.output.lines=merged;
    result.prepared=PrepareLines(result,7);QCOMPARE(result.prepared.size(),merged.size());
    const auto& transform=result.stamp.viewTransform;
    const okuflow::ViewTransform present{.2f,.1f,.5f,.7f,.1f,.05f,.8f,.9f,true};
    for(int i=0;i<merged.size();++i)for(int j=0;j<merged[i].words.size();++j) {
        const auto& word=merged[i].words[j];const auto& prepared=result.prepared[i].words[j];
        QVERIFY(prepared.sceneQuad.valid);
        const auto projected=SceneQuadToView(present,960,540,prepared.sceneQuad);QVERIFY(projected.valid);
        for(int k=0;k<4;++k) {
            const bool firstWord=word.text=="READ";
            const double u=(k==0||k==3)?(firstWord?.095:.445):(firstWord?.165:.515);
            const double v=k<2?0:1;
            const QPointF knownPost(32+256*u+region.x(),64-80*u+80*u*u+(2*v-1)*12+region.y());
            const auto raw=inverse.map(knownPost);const auto restored=H.map(raw);
            QVERIFY(QLineF(restored,knownPost).length()<1e-9);
            QVERIFY(QLineF(word.quadPx.points[k],knownPost).length()<1e-9);
            const double sx=transform.sourceX+(restored.x()/480-transform.destinationX)*transform.sourceWidth/transform.destinationWidth;
            const double sy=transform.sourceY+(restored.y()/260-transform.destinationY)*transform.sourceHeight/transform.destinationHeight;
            const QPointF expected((present.destinationX+(sx-present.sourceX)*present.destinationWidth/present.sourceWidth)*960,
                                   (present.destinationY+(sy-present.sourceY)*present.destinationHeight/present.sourceHeight)*540);
            QVERIFY(QLineF(projected.points[k],expected).length()<1e-7);
        }
    }
    // Reading order uses centerline support, not a tall curved envelope.
    AcceptedLine high,low;high.id=1;low.id=2;
    high.sceneRect={.1,.1,.4,.7};low.sceneRect={.1,.3,.4,.1};
    high.ribbonReadingBand=QRectF(.1,.2,.4,.04);low.ribbonReadingBand=QRectF(.1,.35,.4,.04);
    const auto order=OrderLinesForReading({&low,&high});QCOMPARE(order[0],&high);
}

inline void SourceProofAndPaint() {
    // Real worker preparation/model/paint with scripted CTC output. No models,
    // native window, camera, or inference. The source is NOT the rectified crop.
    auto q=Analytic(80);auto supported=Words(q);auto unsupported=Words(q,true);
    unsupported.words[0].text="CURVED";
    unsupported.words[0].boundsPx.translate(0,70);unsupported.words[0].protectedBoundsPx.translate(0,70);
    unsupported.words[0].quadPx=unsupported.words[0].quadPx.Translated({0,70});
    unsupported.words[0].inkSearchQuadPx=unsupported.words[0].inkSearchQuadPx.Translated({0,70});
    unsupported.words[0].ribbonReadingPointPx+=QPointF(0,70);
    struct Backend final:Recognizer {
        RecognitionOutput output;bool allowed{};std::shared_ptr<std::atomic<int>> contexts;
        BackendStatus Initialize()override{return {BackendState::kReady,"scripted-ctc","",""};}
        void SetCurvedRibbonInput(bool value)override{allowed=value;if(value)++*contexts;}
        RecognitionOutput Recognize(const QImage&,const std::atomic<bool>&)override{return output;}
    };
    auto image=Gradient();const auto stamp=Stamp(image.size());
    auto calls=std::make_shared<std::atomic<int>>();auto backend=std::make_unique<Backend>();backend->contexts=calls;
    backend->output.ok=true;backend->output.lines={supported,unsupported};
    // Keep CTC chords, measuring proof on the original source independently.
    for(auto& line:backend->output.lines)for(auto& word:line.words)word.paddedBounds=false;
    std::optional<RecognitionResult> result;std::atomic<bool> received{};
    RecognitionScheduler scheduler(std::move(backend),[&](auto r){result=std::move(r);received=true;},{});
    QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
    QVERIFY(scheduler.Submit(stamp,image));QTRY_VERIFY(received.load());QVERIFY(result->output.ok);QCOMPARE(calls->load(),1);
    QCOMPARE(result->sourcePixelDigest,uct_test::SourcePixelsDigest(image));
    const auto expected=MakeProbeFrame(image.constBits(),image.width(),image.height(),image.bytesPerLine(),2,stamp);
    QCOMPARE(result->reference.luma,expected.luma);
    OverlayModel model;QCOMPARE(model.AdmitImmutable(*result,stamp,100,nullptr,ImmutableAdmission::Standalone),AdmitVerdict::kAccepted);
    const auto scene=model.SceneWords(100,stamp.frameSeq);QCOMPARE(scene.size(),2);
    QVERIFY(!model.ReadingText(100,stamp.frameSeq).contains("CURVED"));
    const auto screen=model.ScreenLines(100,stamp.frameSeq);QVERIFY(!screen.isEmpty());
    for(const auto& line:screen)QVERIFY(!line.text.contains("CURVED"));
    QCOMPARE(QGuiApplication::platformName(),QStringLiteral("offscreen"));
    uct_test::RegisterFixtureFonts();QWidget target;target.resize(image.size());target.show();
    okuflow::UltraClearTextOverlay overlay(&target,&target);overlay.SetViewTransform(stamp.viewTransform);
    QVector<okuflow::UltraClearTextOverlay::SceneWord> paint;
    for(const auto& word:scene)paint.push_back({word.text,word.sceneRect,word.colors,word.lineId,
        word.protectedSceneRects,word.wordIndex,word.sceneQuad,word.layoutGroupId,word.carvedSceneRects});
    overlay.SetWords(paint);
    QTRY_VERIFY(overlay.PaintedGeometryCurrent());
    for(const auto& word:scene) {
        okuflow::UltraClearTextOverlay::PaintedWordGeometry geometry;
        QVERIFY(overlay.PaintedWord(word.lineId,word.wordIndex,word.text,geometry));QVERIFY(geometry.drawn);
    }
    auto output=image.copy();QPainter painter(&output);
    overlay.CaptureSnapshot(stamp.viewTransform,output.size()).Paint(painter,output.size());painter.end();
    QVERIFY(output!=image);
    // The unsupported long curved word's entire protected source stays ink.
    const auto protection=unsupported.words[0].protectedBoundsPx.toAlignedRect().intersected(image.rect());
    QVERIFY(!protection.isEmpty());
    QCOMPARE(output.copy(protection),image.copy(protection));
    QAccessible::setActive(true);QVERIFY(QAccessible::isActive());
    okuflow::UctScreenTextAccessibility accessibility(&overlay,&target,[&] {
        return okuflow::UctScreenTextSnapshot{true,model.ScreenLines(100,stamp.frameSeq),"en-US"};
    },[]{return std::int64_t(100);});
    accessibility.SetEnabled(true);accessibility.ContentChanged();accessibility.ProcessDueWork();
    const auto* root=accessibility.RootInterface();QVERIFY(root);QVERIFY(root->childCount()>0);
    for(int i=0;i<root->childCount();++i) {
        auto* line=root->child(i);QVERIFY(line);QVERIFY(!line->text(QAccessible::Name).contains("CURVED"));
        auto* text=line->textInterface();QVERIFY(text);
        for(int j=0;j<text->characterCount();++j) {
            if(text->text(j,j+1)==" ")continue;
            const auto box=text->characterRect(j);QVERIFY(!box.isEmpty());
            // Actual positioned text uses the painter's logical-pixel geometry.
            QVERIFY(target.rect().adjusted(-2,-2,2,2).contains(QRect(target.mapFromGlobal(box.topLeft()),box.size())));
        }
    }
}

inline QImage InkImage(bool changed) {
    QImage image(256,96,QImage::Format_ARGB32);image.fill(QColor(242,240,230));
    QPainter p(&image);p.setPen(Qt::NoPen);p.setBrush(QColor(20,24,22));
    for(int i=0;i<9;++i){p.drawRect(28+i*20,30,3,26);p.drawRect(28+i*20,30,12,3);p.drawRect(28+i*20,53,12,3);}
    if(changed)p.drawRect(108,39,12,3);return image;
}
inline void InkOwnershipAndResidual() {
    using namespace current_ink;
    auto image=InkImage(false);const auto stamp=Stamp(image.size());const auto quad=TextQuad::FromRect({24,24,190,40});
    const auto reference=Build(image,stamp,quad);QVERIFY(reference.patch.Valid());
    const auto unchanged=Check(reference,image,stamp);QCOMPARE(unchanged.verdict,Verdict::Unchanged);
    const auto edited=Check(reference,InkImage(true),stamp);QCOMPARE(edited.verdict,Verdict::Changed);
    QVERIFY(edited.worstResidual>unchanged.worstResidual+.1);
    image.fill(Qt::white);QCOMPARE(Check(reference,InkImage(false),stamp).verdict,Verdict::Unchanged); // reference owns pixels
    const auto jittered=[](const QImage& image) {QImage copy(image.size(),image.format());copy.fill(QColor(242,240,230));QPainter p(&copy);p.drawImage(QPoint(2,1),image);return copy;};
    const auto jitter=Check(reference,jittered(InkImage(false)),stamp);QCOMPARE(jitter.verdict,Verdict::Unchanged);QCOMPARE(jitter.jitterPx,QPointF(2,1));
    QCOMPARE(Check(reference,jittered(InkImage(true)),stamp).verdict,Verdict::Changed);
    // Independent defocus controls: matched blur may only demote a sharp
    // Changed to Unverifiable; it can never manufacture Unchanged.
    const auto defocused=[](const QImage& input) {auto out=input;for(int y=1;y<95;++y)for(int x=1;x<255;++x){int r=0,g=0,b=0;for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const auto c=input.pixelColor(x+dx,y+dy);r+=c.red();g+=c.green();b+=c.blue();}out.setPixelColor(x,y,QColor(r/9,g/9,b/9));}return out;};
    const auto defocus=Check(reference,defocused(InkImage(false)),stamp);
    QVERIFY2(defocus.verdict!=Verdict::Changed,qPrintable(QString("sharp=%1 softened=%2 shift=%3,%4")
        .arg(defocus.worstResidual).arg(defocus.blurredResidual).arg(defocus.jitterPx.x()).arg(defocus.jitterPx.y())));
    QVERIFY(Check(reference,defocused(InkImage(true)),stamp).verdict!=Verdict::Unchanged);
    auto stale=stamp;++stale.geometryGeneration;QCOMPARE(Check(reference,InkImage(false),stale).verdict,Verdict::Unverifiable);
    stale=stamp;stale.temporallySmoothed=true;QCOMPARE(Check(reference,InkImage(false),stale).verdict,Verdict::Unverifiable);
    QVERIFY(!Capture(InkImage(false).scaled(128,48),stamp,quad).Valid());
    QVERIFY(!Capture(InkImage(false),stamp,TextQuad::FromRect({0,0,20,20})).Valid());
    QVERIFY(!Capture(InkImage(false),stamp,TextQuad::FromRect({1,1,600,40})).Valid());
    Patch invalid=reference.patch;invalid.luma[0]=std::numeric_limits<float>::quiet_NaN();
    QCOMPARE(Compare(reference.patch,invalid).verdict,Verdict::Unverifiable);
}
inline void ExportTypesetFixture(const QString& root) {
    if(root.isEmpty())return;
    uct_test::RegisterFixtureFonts();
    QFile fontFile("C:/Windows/Fonts/arial.ttf");QVERIFY(fontFile.open(QIODevice::ReadOnly));
    const auto fontHash=QCryptographicHash::hash(fontFile.readAll(),QCryptographicHash::Sha256).toHex();
    QFont font("Arial");font.setPixelSize(26);font.setStyleStrategy(QFont::PreferAntialias);
    const QString text="Curved page reading 18";
    QImage local(448,48,QImage::Format_ARGB32);local.fill(QColor(246,244,236));
    {QPainter p(&local);p.setFont(font);p.setPen(QColor(18,18,22));p.drawText(QPointF(8,34),text);}
    QImage curved(512,200,QImage::Format_ARGB32),straight(curved.size(),curved.format());
    curved.fill(QColor(246,244,236));straight.fill(QColor(246,244,236));
    {QPainter p(&straight);p.drawImage(QPoint(32,76),local);}
    // Known inverse map of an original source-authored line: x=32+448u,
    // y=100-96u+96u^2+48(v-.5). Only this test generator warps pixels;
    // production sampling always returns to the original curved image.
    for(int y=0;y<200;++y)for(int x=32;x<480;++x) {
        const double u=(x+.5-32)/448.,center=100-96*u+96*u*u;
        const double ly=y+.5-center+24-.5;const int iy=int(std::floor(ly));
        if(iy<0||iy>=47)continue;const double fraction=ly-iy;
        const auto a=local.pixelColor(x-32,iy),b=local.pixelColor(x-32,iy+1);
        curved.setPixelColor(x,y,QColor(int(a.red()*(1-fraction)+b.red()*fraction+.5),
            int(a.green()*(1-fraction)+b.green()*fraction+.5),int(a.blue()*(1-fraction)+b.blue()*fraction+.5)));
    }
    QVERIFY(curved!=straight);QVERIFY(QDir().mkpath(root));
    QVERIFY(curved.save(QDir(root).filePath("curve-typeset.png")));QVERIFY(straight.save(QDir(root).filePath("straight-typeset.png")));
    for(const auto* name:{"curve-typeset.png.truth.txt","straight-typeset.png.truth.txt"}) {
        QSaveFile truth(QDir(root).filePath(QString::fromLatin1(name)));QVERIFY(truth.open(QIODevice::WriteOnly));
        const auto bytes=(text+"\n").toUtf8();QCOMPARE(truth.write(bytes),bytes.size());QVERIFY(truth.commit());
    }
    QJsonArray words;const QFontMetricsF metrics(font);double start=8;
    for(const auto& word:text.split(' ')) {
        const double width=metrics.horizontalAdvance(word),u0=start/448,u1=(start+width)/448;
        words.push_back(QJsonObject{{"text",word},{"u0",u0},{"u1",u1},{"sourceChordSagittaPx",96*std::pow(u1-u0,2)/4}});
        start+=width+metrics.horizontalAdvance(' ');
    }
    const QJsonObject manifest{{"schema",1},{"originalAuthoredSynthetic",true},{"inference",false},
        {"fontPath","C:/Windows/Fonts/arial.ttf"},{"fontSha256",QString::fromLatin1(fontHash)},
        {"fontPixelSize",26},{"text",text},{"map","x=32+448*u; y=100-96*u+96*u*u+48*(v-.5)"},
        {"curveActiveRowSha256",QString::fromLatin1(uct_test::SourcePixelsDigest(curved).toHex())},
        {"straightActiveRowSha256",QString::fromLatin1(uct_test::SourcePixelsDigest(straight).toHex())},{"words",words}};
    QSaveFile file(QDir(root).filePath("ribbon-fixture-inputs.json"));QVERIFY(file.open(QIODevice::WriteOnly));
    const auto bytes=QJsonDocument(manifest).toJson();QCOMPARE(file.write(bytes),bytes.size());QVERIFY(file.commit());
}
inline void HeldOutInkDiagnostic() {
    using namespace current_ink;
    uct_test::RegisterFixtureFonts();
    QJsonArray rows;int falseAccepts=0,falseHides=0,unverifiable=0,changedCount=0,unchangedCount=0;
    // These fonts/sizes and nuisance transforms are held out of the 12-edit
    // Consolas corpus. Reporting completes even when the verifier makes errors.
    for(const auto* fontPath:{"C:/Windows/Fonts/arial.ttf","C:/Windows/Fonts/segoeui.ttf"}) {
        QFile fontFile(QString::fromLatin1(fontPath));QVERIFY(fontFile.open(QIODevice::ReadOnly));
        const auto fontHash=QCryptographicHash::hash(fontFile.readAll(),QCryptographicHash::Sha256).toHex();
        const int id=QFontDatabase::addApplicationFont(QString::fromLatin1(fontPath));QVERIFY(id>=0);
        const auto family=QFontDatabase::applicationFontFamilies(id).first();
        for(int size:{19,31}) {
            QFont font(family);font.setPixelSize(size);
            auto render=[&](bool changed) {QImage image(256,96,QImage::Format_ARGB32);image.fill(QColor(242,240,230));QPainter p(&image);p.setFont(font);p.setPen(QColor(20,24,22));p.drawText(QPointF(28,60),changed?QStringLiteral("Rate 19"):QStringLiteral("Rate 18"));return image;};
            const auto original=render(false);const auto stamp=Stamp(original.size());
            const auto reference=Build(original,stamp,TextQuad::FromRect({24,24,190,46}));QVERIFY(reference.patch.Valid());
            for(int nuisance=0;nuisance<6;++nuisance)for(bool changed:{false,true}) {
                auto current=render(changed);
                if(nuisance==1||nuisance==2||nuisance==4) {
                    QImage transformed(current.size(),current.format());transformed.fill(QColor(242,240,230));
                    QPainter p(&transformed);p.setRenderHint(QPainter::SmoothPixmapTransform);
                    if(nuisance==1)p.translate(.5,-.5);
                    if(nuisance==2){p.translate(128,48);p.scale(1.025,1.025);p.translate(-128,-48);}
                    if(nuisance==4)p.translate(2,1);
                    p.drawImage(QPointF(),current);p.end();current=transformed;
                }
                if(nuisance==1) {
                    // Actual half-pixel bilinear jitter. QPainter may round a
                    // translated drawImage to integer pixels on this backend.
                    const auto input=render(changed);
                    for(int y=1;y<95;++y)for(int x=1;x<255;++x) {
                        int r=0,g=0,b=0;
                        for(int dy=0;dy<=1;++dy)for(int dx=-1;dx<=0;++dx) {
                            const auto c=input.pixelColor(x+dx,y+dy);r+=c.red();g+=c.green();b+=c.blue();
                        }current.setPixelColor(x,y,QColor(r/4,g/4,b/4));
                    }
                    QVERIFY(current!=input);
                }
                if(nuisance==3) {
                    const auto before=current;for(int y=1;y<95;++y)for(int x=1;x<255;++x) {
                        int r=0,g=0,b=0;for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const auto c=before.pixelColor(x+dx,y+dy);r+=c.red();g+=c.green();b+=c.blue();}
                        current.setPixelColor(x,y,QColor(r/9,g/9,b/9));
                    }
                }
                if(nuisance==5)for(int y=0;y<96;++y)for(int x=0;x<256;++x) {
                    const auto c=current.pixelColor(x,y);current.setPixelColor(x,y,QColor(int(c.red()*.8+25),int(c.green()*.8+25),int(c.blue()*.8+25)));
                }
                const auto verdict=Check(reference,current,stamp);
                if(changed){++changedCount;falseAccepts+=verdict.verdict==Verdict::Unchanged;}
                else{++unchangedCount;falseHides+=verdict.verdict==Verdict::Changed;}
                unverifiable+=verdict.verdict==Verdict::Unverifiable;
                rows.push_back(QJsonObject{{"font",family},{"fontSha256",QString::fromLatin1(fontHash)},{"size",size},
                    {"nuisance",nuisance},{"changed",changed},{"verdict",int(verdict.verdict)},{"reason",int(verdict.reason)},
                    {"worstResidual",verdict.worstResidual},{"blurredResidual",verdict.blurredResidual},{"referencePixelSha256",QString::fromLatin1(uct_test::SourcePixelsDigest(original).toHex())},
                    {"currentPixelSha256",QString::fromLatin1(uct_test::SourcePixelsDigest(current).toHex())}});
            }
        }
    }
    QCOMPARE(rows.size(),48);
    const QJsonObject report{{"schema",1},{"diagnosticOnly",true},{"nativeGpuCoverage",false},{"changed",changedCount},
        {"unchanged",unchangedCount},{"falseAccepts",falseAccepts},{"falseHides",falseHides},{"unverifiable",unverifiable},{"rows",rows}};
    const auto root=qEnvironmentVariable("OKUFLOW_UCT_RIBBON_EVIDENCE");
    ExportTypesetFixture(root);
    if(!root.isEmpty()) {QVERIFY(QDir().mkpath(root));QSaveFile file(QDir(root).filePath("ink-held-out.json"));QVERIFY(file.open(QIODevice::WriteOnly));const auto bytes=QJsonDocument(report).toJson();QCOMPARE(file.write(bytes),bytes.size());QVERIFY(file.commit());}
    qInfo().noquote()<<"UCT_INK_HELD_OUT"<<QJsonDocument(report).toJson(QJsonDocument::Compact);
}

inline RecognizedWord MergeWord(TokenDisposition disposition,bool ribbon,const QRectF& protection) {
    RecognizedWord word;word.text="read";word.boundsPx={40,40,100,24};word.paddedBounds=true;
    word.inkSearchPx=word.boundsPx;word.disposition=disposition;word.confidence=.99;
    word.protectedBoundsPx=protection;word.quadPx=TextQuad::FromRect(word.boundsPx);
    word.inkSearchQuadPx=word.quadPx;word.fromRibbon=ribbon;word.ribbonErrorPx=ribbon?-1:0;
    word.ribbonReadingPointPx=word.boundsPx.center();return word;
}
inline void MergeProtectionAndWindows() {
    const QRectF envelope(10,20,400,60),own(40,40,100,24);
    const QRect whole(0,0,480,260);const QSize source(480,260);
    {   // Candidate first: the refused ribbon word loses its slot, not its mask.
        QVector<RecognizedLine> output;RecognizedLine candidate,refused;
        candidate.words={MergeWord(TokenDisposition::kCandidate,false,own)};
        refused.words={MergeWord(TokenDisposition::kInvalidGeometry,true,envelope)};
        PaddleMergeRecognition(output,candidate,whole,source);
        PaddleMergeRecognition(output,refused,whole,source);
        QCOMPARE(output.size(),1);QCOMPARE(output[0].words.size(),1);
        QCOMPARE(output[0].words[0].disposition,TokenDisposition::kInvalidGeometry);
        QVERIFY(output[0].words[0].protectedBoundsPx.contains(envelope));
    }
    {   // Refused first: the replacing candidate inherits the envelope.
        QVector<RecognizedLine> output;RecognizedLine candidate,refused;
        candidate.words={MergeWord(TokenDisposition::kCandidate,false,own)};
        refused.words={MergeWord(TokenDisposition::kInvalidGeometry,true,envelope)};
        PaddleMergeRecognition(output,refused,whole,source);
        PaddleMergeRecognition(output,candidate,whole,source);
        QCOMPARE(output.size(),1);QCOMPARE(output[0].words.size(),1);
        QCOMPARE(output[0].words[0].disposition,TokenDisposition::kInvalidGeometry);
        QVERIFY(output[0].words[0].protectedBoundsPx.contains(envelope));
    }
    {   // Baseline replacement keeps exactly the winner's own protection.
        QVector<RecognizedLine> output;RecognizedLine first,second;
        first.words={MergeWord(TokenDisposition::kCandidate,false,envelope)};
        second.words={MergeWord(TokenDisposition::kCandidate,false,own)};
        PaddleMergeRecognition(output,first,whole,source);
        PaddleMergeRecognition(output,second,whole,source);
        QCOMPARE(output[0].words.size(),1);QCOMPARE(output[0].words[0].protectedBoundsPx,own);
    }
    {   // The carrier remains invalid through ordinary replacement and scene preparation.
        QVector<RecognizedLine> output;RecognizedLine refused,candidate;
        refused.words={MergeWord(TokenDisposition::kInvalidGeometry,true,envelope)};
        candidate.words={MergeWord(TokenDisposition::kCandidate,false,own)};
        PaddleMergeRecognition(output,refused,whole,source);
        for(int repeat=0;repeat<3;++repeat)PaddleMergeRecognition(output,candidate,whole,source);
        RecognitionResult result;result.stamp=Stamp(source);result.output.ok=true;result.output.lines=output;
        const auto prepared=PrepareLines(result,7);
        QCOMPARE(prepared.size(),1);QCOMPARE(prepared[0].words.size(),1);
        QCOMPARE(prepared[0].words[0].disposition,TokenDisposition::kInvalidGeometry);
        QVERIFY(output[0].words[0].protectedBoundsPx.contains(envelope));
    }
    // Windowed decode against the independent analytic map x=32+2000t,
    // y=64-80t+80t*t +/- 12. Production At/windowing is not its own oracle.
    PaddleQuad line{{QPointF(32,52),QPointF(2032,52),QPointF(2032,76),QPointF(32,76)},.98f};
    line.ribbon={32,64,2000,0,0,1,0,-80,80,12,0,0,1,true};
    const auto windows=PaddleRecognitionWindows(line);QVERIFY(windows.size()>1);
    const auto& window=windows[1];QVERIFY(window.ribbon.valid);
    const auto decoded=Words(window);QVERIFY(!decoded.words.isEmpty());
    const double t0=window.ribbon.start+(window.ribbon.end-window.ribbon.start)*.095;
    const double t1=window.ribbon.start+(window.ribbon.end-window.ribbon.start)*.165;
    const auto& word=decoded.words[0];
    QVERIFY(std::abs(word.quadPx.points[0].x()-(32+2000*t0))<1e-9);
    QVERIFY(std::abs(word.quadPx.points[0].y()-(64-80*t0+80*t0*t0-12))<1e-9);
    QVERIFY(std::abs(word.quadPx.points[1].x()-(32+2000*t1))<1e-9);
    QVERIFY(std::abs(word.quadPx.points[2].y()-(64-80*t1+80*t1*t1+12))<1e-9);
    // Bin fill order (CPU component order vs GPU scan order) may change only
    // summation rounding: support counts and extrema stay exact.
    std::vector<std::pair<double,double>> samples;
    for(int repeat=0;repeat<40;++repeat)for(int i=0;i<ribbon::kBins;++i) {
        const double middle=(i+.5)/ribbon::kBins;
        for(double t:{middle-.01,middle,middle+.01}) {
            const double center=80-80*t+80*t*t;
            samples.push_back({t,center-5});samples.push_back({t,center+5});
        }
    }
    ribbon::Bin forward[ribbon::kBins]{},reverse[ribbon::kBins]{};
    for(const auto& sample:samples)ribbon::Add(forward,sample.first,sample.second);
    for(auto it=samples.rbegin();it!=samples.rend();++it)ribbon::Add(reverse,it->first,it->second);
    for(int i=0;i<ribbon::kBins;++i) {
        QCOMPARE(forward[i].count,reverse[i].count);QCOMPARE(forward[i].low,reverse[i].low);
        QCOMPARE(forward[i].high,reverse[i].high);
    }
    const auto fitA=ribbon::Fit(forward,20,420,1,0,1,1,500,200);
    const auto fitB=ribbon::Fit(reverse,20,420,1,0,1,1,500,200);
    QVERIFY(fitA.valid&&fitB.valid);
    QVERIFY(std::abs(fitA.c-fitB.c)<1e-6);QVERIFY(std::abs(fitA.h0-fitB.h0)<1e-6);
}
inline void PrimitiveBenchmark() {
    if(qEnvironmentVariable("OKUFLOW_UCT_RIBBON_BENCHMARK")!="1")QSKIP("Explicit model-free A/B benchmark not selected.");
    const auto root=qEnvironmentVariable("OKUFLOW_UCT_RIBBON_EVIDENCE");QVERIFY(!root.isEmpty());QVERIFY(QDir().mkpath(root));
    const auto image=Gradient();const auto map=DetectorMap(true);std::atomic<bool> cancelled{};
    const auto inkBefore=InkImage(false),inkAfter=InkImage(true);const auto stamp=Stamp(inkBefore.size());
    const auto word=TextQuad::FromRect({24,24,190,40});
    const auto reference=current_ink::Build(inkBefore,stamp,word);
    const auto oldProbe=MakeProbeFrame(inkBefore.constBits(),256,96,inkBefore.bytesPerLine(),1,stamp);
    const auto newProbe=MakeProbeFrame(inkAfter.constBits(),256,96,inkAfter.bytesPerLine(),1,stamp);
    const auto ncc=BuildReferencePatch(oldProbe,{24./256,24./96,190./256,40./96});QVERIFY(ncc.valid);
    QJsonArray rows;
    for(int visit=0;visit<36;++visit) for(int arm=0;arm<2;++arm) {
        const bool ribbonArm=((visit%2)==0)?arm==1:arm==0; // alternating AB/BA
        QElapsedTimer whole,timer;whole.start();timer.start();
        const auto detected=PaddleDetect(map,320,160,{320,160},cancelled,ribbonArm);
        const double detectMs=timer.nsecsElapsed()/1e6;timer.restart();
        QCryptographicHash cropHash(QCryptographicHash::Sha256);int windows=0,words=0,refused=0;
        double rectifyMs=0,decodeMs=0;QVector<RecognizedLine> merged;
        for(const auto& line:detected)for(const auto& crop:PaddleRecognitionWindows(line)) {
            ++windows;timer.restart();const auto pixels=PaddleRectify(image,crop,1024,512);
            rectifyMs+=timer.nsecsElapsed()/1e6;
            cropHash.addData(QByteArrayView(reinterpret_cast<const char*>(pixels.data()),qsizetype(pixels.size()*sizeof(float))));
            timer.restart();auto decoded=Words(crop);words+=decoded.words.size();
            for(const auto& word:decoded.words)refused+=word.disposition==TokenDisposition::kInvalidGeometry;
            PaddleMergeRecognition(merged,std::move(decoded),{0,0,384,160},image.size());decodeMs+=timer.nsecsElapsed()/1e6;
        }
        const double primitiveWholeMs=whole.nsecsElapsed()/1e6;
        timer.restart();const auto old=MatchPatch(ncc,newProbe,{});const double oldMs=timer.nsecsElapsed()/1e6;
        timer.restart();const auto ink=current_ink::Check(reference,inkAfter,stamp);const double newMs=timer.nsecsElapsed()/1e6;
        rows.push_back(QJsonObject{{"visit",visit},{"warmup",visit<5},{"ribbon",ribbonArm},{"detectorFitMs",detectMs},
            {"rectifyMs",rectifyMs},{"ctcMapMergeMs",decodeMs},{"primitiveWithDiagnosticHashWallMs",primitiveWholeMs},{"primitiveStageSumMs",detectMs+rectifyMs+decodeMs},
            {"nccMs",oldMs},{"currentInkMs",newMs},{"nccMatched",old.matched},{"inkVerdict",int(ink.verdict)},
            {"detected",detected.size()},{"windows",windows},{"words",words},{"geometryRefused",refused},
            {"cropFloatBytesSha256",QString::fromLatin1(cropHash.result().toHex())}});
    }
    const auto mapBytes=QByteArrayView(reinterpret_cast<const char*>(map.data()),qsizetype(map.size()*sizeof(float)));
    const QJsonObject report{{"schema",1},{"sameProductionPrimitives",true},{"inference",false},{"wholeWorkerTiming",false},
        {"sourcePixelsSha256",QString::fromLatin1(uct_test::SourcePixelsDigest(image).toHex())},
        {"detectorMapFloatBytesSha256",QString::fromLatin1(QCryptographicHash::hash(mapBytes,QCryptographicHash::Sha256).toHex())},
        {"inkBeforePixelsSha256",QString::fromLatin1(uct_test::SourcePixelsDigest(inkBefore).toHex())},
        {"inkAfterPixelsSha256",QString::fromLatin1(uct_test::SourcePixelsDigest(inkAfter).toHex())},{"rows",rows}};
    QSaveFile file(QDir(root).filePath("ribbon-ink-primitives-ab.json"));QVERIFY(file.open(QIODevice::WriteOnly));
    const auto bytes=QJsonDocument(report).toJson();QCOMPARE(file.write(bytes),bytes.size());QVERIFY(file.commit());
    qInfo().noquote()<<"UCT_RIBBON_PRIMITIVES_AB"<<file.fileName();
}
} // namespace uct_ribbon_test
