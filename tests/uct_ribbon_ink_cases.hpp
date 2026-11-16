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
#include <QTemporaryDir>
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
inline void EndpointPadding() {
    ribbon::Bin bins[ribbon::kBins]{};
    for(int i=0;i<ribbon::kBins;++i) {
        const double t=(i+.5)/ribbon::kBins,center=80-80*t+80*t*t;
        ribbon::Add(bins,t,center-5);ribbon::Add(bins,t,center+5);
    }
    const auto old=ribbon::Fit(bins,20,420,1,0,1,1,500,200,false);
    const auto padded=ribbon::Fit(bins,20,420,1,0,1,1,500,200);
    QVERIFY(old.valid);QVERIFY(padded.valid);QVERIFY(padded.Inside(500,200));
    QCOMPARE(padded.start,0.0);QCOMPARE(padded.end,1.0);
    const double margin=400*11*1.4/(2*(400+11)),delta=margin/400;
    for(int i=0;i<=32;++i) {
        const double u=i/32.0,t=-delta+(1+2*delta)*u;
        const auto p=padded.At(u,.5);
        QVERIFY(std::abs(p.x-(20+400*t))<1e-9);
        QVERIFY(std::abs(p.y-(80-80*t+80*t*t))<1e-9);
    }
    QVERIFY(std::abs(padded.Error(.2,.3)-80*std::pow((1+2*delta)*.1,2)/4)<1e-10);
    QImage image(500,200,QImage::Format_ARGB32);image.fill(Qt::white);
    for(int y=70;y<90;++y)for(int x:{13,14,15,16,17,18,423,424,425,426})image.setPixelColor(x,y,Qt::red);
    PaddleQuad q{{QPointF(20,40),QPointF(420,40),QPointF(420,120),QPointF(20,120)},.99f};
    q.ribbon=old;const auto before=PaddleRectify(image,q,512,512);
    q.ribbon=padded;const auto after=PaddleRectify(image,q,512,512);
    const auto plane=std::size_t(512*48);
    QVERIFY(std::all_of(before.begin(),before.begin()+plane,[](float v){return v==1.f;}));
    bool left=false,right=false;
    for(int y=0;y<48;++y)for(int x=0;x<512;++x)if(after[y*512+x]<0){left|=x<256;right|=x>=256;}
    QVERIFY(left);QVERIFY(right);
    QVERIFY(ribbon::Fit(bins,20,420,1,0,1,1,425,200,false).valid);
    const auto edge=ribbon::Fit(bins,20,420,1,0,1,1,425,200);
    QVERIFY(!edge.valid);QVERIFY(edge.unsupported); // no clamped/extrapolated out-of-source pixels
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
    stamp.immutableSelectedInput=false;QVERIFY(PaddleRibbonInputAllowed(stamp));
    stamp.temporallySmoothed=true;QVERIFY(PaddleRibbonInputAllowed(stamp));
    stamp.temporallySmoothed=false;stamp.liveCameraCapture=true;QVERIFY(PaddleRibbonInputAllowed(stamp));
    stamp.holdDisplay=HoldDisplay::Live;stamp.holdAcquisition=stamp.frameSeq;QVERIFY(PaddleRibbonInputAllowed(stamp));
    stamp.holdAcquisition=stamp.frameSeq+1;QVERIFY(!PaddleRibbonInputAllowed(stamp));
    stamp.holdDisplay=HoldDisplay::Held;stamp.holdAcquisition=stamp.frameSeq-1;QVERIFY(PaddleRibbonInputAllowed(stamp));
    stamp.holdAcquisition=stamp.frameSeq;QVERIFY(!PaddleRibbonInputAllowed(stamp));
    stamp.holdDisplay=HoldDisplay::Unknown;QVERIFY(!PaddleRibbonInputAllowed(stamp));
    stamp.holdDisplay=HoldDisplay::Crossfade;QVERIFY(!PaddleRibbonInputAllowed(stamp));
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

inline void SmallWordsAndPaintReasons() {
    RecognitionResult result;result.stamp=Stamp(Gradient().size());result.output.ok=true;
    for (const double height : {4.99,5.0,6.0}) {
        RecognizedWord word;word.text="read";word.boundsPx={30,30,40,height};
        word.quadPx=TextQuad::FromRect(word.boundsPx);word.fromRibbon=true;
        result.output.lines.push_back({{word}});
    }
    const auto prepared=PrepareLines(result,ModelTuning{}.minimumWordHeightPx);
    QCOMPARE(prepared[0].words[0].disposition,TokenDisposition::kTooSmall);
    QCOMPARE(prepared[1].words[0].disposition,TokenDisposition::kCandidate);
    QCOMPARE(prepared[2].words[0].disposition,TokenDisposition::kCandidate);
    const auto control=PrepareLines(result,7);
    for(const auto& line:control)QCOMPARE(line.words[0].disposition,TokenDisposition::kTooSmall);
    // The change admits small OCR candidates; the painter retains its own
    // projected-size, collision and source-ink decisions.
    QCOMPARE(QGuiApplication::platformName(),QStringLiteral("offscreen"));
    uct_test::RegisterFixtureFonts();QWidget target;target.resize(384,160);target.show();
    using Overlay=okuflow::UltraClearTextOverlay;using Reason=Overlay::PaintOmission;
    Overlay overlay(&target,&target);overlay.SetViewTransform(result.stamp.viewTransform);
    const auto make=[](QRectF pixels,int index,QString text="read") {
        const QRectF scene(pixels.x()/384,pixels.y()/160,pixels.width()/384,pixels.height()/160);
        Overlay::SceneWord word{text,scene,{Qt::black,Qt::white},1,{},index};
        word.sceneQuad=TextQuad::FromRect(scene);return word;
    };
    const auto check=[&](QVector<Overlay::SceneWord> words,QVector<Reason> expected) {
        const auto generation=overlay.PaintedGeneration();
        overlay.SetWords(words);QTRY_VERIFY(overlay.PaintedGeneration()>generation);
        QVERIFY(overlay.PaintedGeometryCurrent());
        QCOMPARE(words.size(),expected.size());
        for(int i=0;i<words.size();++i) {
            Overlay::PaintedWordGeometry actual;
            QVERIFY(overlay.PaintedWord(words[i].lineId,words[i].wordIndex,words[i].text,actual));
            QCOMPARE(actual.omission,expected[i]);QCOMPARE(actual.drawn,expected[i]==Reason::kNone);
        }
    };
    const auto word=make({30,30,100,20},0);
    check({word},{Reason::kNone});
    auto protectedWord=word;protectedWord.protectedSceneRects={word.sceneRect};
    check({protectedWord},{Reason::kProtectedInk});
    check({word,make({30,30,100,20},1,"ink")},{Reason::kCollision,Reason::kCollision});
    check({word,make({30,30,100,20},1)},{Reason::kNone,Reason::kDuplicate});
    check({make({500,30,100,20},0)},{Reason::kClipped});
    check({make({30,30,100,5},0)},{Reason::kProjectedSize});
    // The same five source pixels become drawable when magnified.
    auto zoom=result.stamp.viewTransform;zoom.sourceWidth=.5;zoom.sourceHeight=.5;
    overlay.SetViewTransform(zoom);
    check({make({30,30,100,5},0)},{Reason::kNone});
}

inline void AllFrameWorkerContexts() {
    struct Backend final : Recognizer {
        std::atomic<int> calls{};std::atomic<bool> enabled{};RecognitionOutput output;
        BackendStatus Initialize()override{return {BackendState::kReady,"ribbon-context-fixture","",""};}
        void SetCurvedRibbonInput(bool value)override{enabled=value;}
        RecognitionOutput Recognize(const QImage&,const std::atomic<bool>&)override{
            if(!enabled.load()){RecognitionOutput refused;refused.error="Missing curved context";return refused;}
            ++calls;return output;
        }
    };
    struct Paired final : DeviceImage {
        QImage current;mutable std::atomic<int> validations{};
        explicit Paired(QImage image):current(std::move(image)){
            width=current.width();height=current.height();pitch=current.bytesPerLine();kind=AnalysisKind::TemporallySmoothed;
        }
        const void* Data()const override{return this;}
        bool WaitReady(void*,QString&)const override{return true;}
        QImage Readback(const std::atomic<bool>&,QString&)const override{return current;}
        QImage ReadValidation(const std::atomic<bool>&,QString&)const override{++validations;return current;}
    };
    auto backend=std::make_unique<Backend>();auto* observed=backend.get();
    backend->output.ok=true;backend->output.lines={Words(Analytic())};
    for(auto& word:backend->output.lines[0].words)word.paddedBounds=false;
    RecognizedWord small;small.text="small";small.boundsPx={30,30,40,5};
    small.quadPx=TextQuad::FromRect(small.boundsPx);small.fromRibbon=true;
    backend->output.lines.push_back({{small}});
    auto image=Gradient();
    // The five-pixel word still needs real contrast for paired-current proof;
    // a smooth gradient over this small patch is intentionally untrackable.
    for(int y=30;y<35;++y)for(int x=30;x<70;++x)
        image.setPixel(x,y,((x/4)%2)?qRgb(255,255,255):qRgb(0,0,0));
    auto stamp=Stamp(image.size());stamp.immutableSelectedInput=false;
    RecognitionResult result;std::atomic<bool> received{};
    RecognitionScheduler scheduler(std::move(backend),[&](auto value){result=std::move(value);received=true;},{});
    QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
    // Real scheduler call-scoping on live camera and advancing media, with no
    // selected-frame flag. The original owned image still supplies references.
    for(bool live:{true,false}){
        stamp.liveCameraCapture=live;++stamp.frameSeq;received=false;
        QVERIFY(scheduler.Submit(stamp,image));QTRY_VERIFY(received.load());QVERIFY(result.output.ok);
        QCOMPARE(result.prepared.back().words[0].disposition,TokenDisposition::kCandidate);
        QVERIFY(!observed->enabled.load());
        const auto original=MakeProbeFrame(image.constBits(),image.width(),image.height(),image.bytesPerLine(),2,stamp);
        QCOMPARE(result.reference.luma,original.luma);
    }
    // Paired Temporal Smooth retains its single-current-frame validation path.
    auto paired=std::make_shared<Paired>(image);stamp.temporallySmoothed=true;++stamp.frameSeq;received=false;
    paired->sourceEpoch=stamp.sourceEpoch;paired->frameSequence=stamp.frameSeq;
    QVERIFY(scheduler.SubmitDevice(stamp,paired));QTRY_VERIFY(received.load());QVERIFY(result.output.ok);
    QCOMPARE(result.prepared.back().words[0].disposition,TokenDisposition::kCandidate);
    QCOMPARE(paired->validations.load(),1);QVERIFY(!result.currentInputMatches.isEmpty());
    for(bool matches:result.currentInputMatches)QVERIFY(matches);
    QCOMPARE(observed->calls.load(),3);QVERIFY(!observed->enabled.load());
    // Unreadable provenance never enters recognition, regardless of mode.
    stamp.holdDisplay=HoldDisplay::Unknown;++stamp.frameSeq;received=false;
    QVERIFY(scheduler.Submit(stamp,image));QTRY_VERIFY(received.load());
    QVERIFY(result.output.compositeInput);QVERIFY(!result.output.ok);QCOMPARE(observed->calls.load(),3);
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
// Envelope of a detector component along its PCA axis: centre
// a + b t + bow * 4 t (t - 1) ... (sagitta `sagitta` at the middle),
// half height `half`. One sample pair per position, `per` positions per bin.
inline void BowedBins(ribbon::Bin* bins, double sagitta, double half, double slope = 0, int per = 3) {
    for(int i=0;i<ribbon::kBins;++i)bins[i]={};
    for(int i=0;i<ribbon::kBins;++i)for(int k=0;k<per;++k) {
        const double t=(i+(k+.5)/per)/ribbon::kBins,center=100+slope*t-4*sagitta*t*(1-t);
        ribbon::Add(bins,t,center-half);ribbon::Add(bins,t,center+half);
    }
}
inline bool SameFit(const ribbon::Geometry& a,const ribbon::Geometry& b) {
    const double fa[]{a.ox,a.oy,a.ux,a.uy,a.vx,a.vy,a.a,a.b,a.c,a.h0,a.h1,a.start,a.end,
                      a.protectLeft,a.protectTop,a.protectRight,a.protectBottom};
    const double fb[]{b.ox,b.oy,b.ux,b.uy,b.vx,b.vy,b.a,b.b,b.c,b.h0,b.h1,b.start,b.end,
                      b.protectLeft,b.protectTop,b.protectRight,b.protectBottom};
    return std::memcmp(fa,fb,sizeof(fa))==0 && a.valid==b.valid && a.unsupported==b.unsupported &&
           a.outcome==b.outcome && std::memcmp(&a.bow,&b.bow,sizeof(a.bow))==0;
}
// Round 12: the straight-line bound scaled by length (private experiment).
inline void StraightSpanBound() {
    constexpr double K=20;
    // Lines up to K heights keep the established bound exactly; longer lines
    // scale it linearly, and only then.
    for(double height:{3.,5.,9.375,12.,40.})for(double heights:{2.,10.,19.99,20.,20.01,45.,200.}) {
        const double length=heights*height,established=std::max(.75,.08*height);
        QCOMPARE(ribbon::StraightBound(height,length,0),established);
        const double scaled=ribbon::StraightBound(height,length,K);
        if(length<=K*height)QCOMPARE(scaled,established);
        else {
            // Only the relative part scales; the 0.75 px floor never does, so
            // small type is never judged more loosely than 8% of its height
            // times the length factor, or the fixed floor.
            const double relative=.08*height*length/(K*height);
            QVERIFY(std::abs(scaled-std::max(.75,relative))<1e-12*scaled);
            QVERIFY(scaled<=std::max(.75,established*length/(K*height))*(1+1e-12));
        }
        // What the scaled bound guarantees for a parabola at the bound: each
        // K-height piece departs from its own chord by at most the established
        // bound, and the end angle (4 sagitta / length) stays at or below the
        // one the established bound allows on a K-height line.
        const double piece=std::min(length,K*height);
        QVERIFY(scaled*(piece/length)*(piece/length)<=established*(1+1e-12));
        QVERIFY(4*scaled/length<=4*established/piece*(1+1e-12));
    }
    QCOMPARE(ribbon::StraightBound(10,1000,-1),.8); // non-positive span: shipped rule
    // Round 12b: at a detector height of 5 px and 100 heights the bound was
    // 0.75 px x 5 = 0.75 h; now it is 0.4 h (8% x 5), and the cap holds every
    // length to kStraightMaximumBow.
    QVERIFY(std::abs(ribbon::StraightBound(5,500,K)-2.)<1e-12);
    ribbon::Bin bins[ribbon::kBins];
    const ribbon::StraightRule before{K,false},refusedOnly{K,true};
    // A long flat line with the small fitted bow seen on flat pages (0.15 of
    // its height over 90 heights): a ribbon today, the rectangle path with
    // `before`; `refusedOnly` keeps a valid ribbon.
    const double half=4.5,height=2*half+1,length=90*height;
    BowedBins(bins,.15*height,half);
    const auto today=ribbon::Fit(bins,20,20+length,1,0,1,1,2000,400);
    QVERIFY(today.valid);QCOMPARE(int(today.outcome),int(ribbon::kFitted));
    QVERIFY(std::abs(today.bow-.15)<.02);
    const auto relieved=ribbon::Fit(bins,20,20+length,1,0,1,1,2000,400,true,before);
    QVERIFY(!relieved.valid);QVERIFY(!relieved.unsupported);QCOMPARE(int(relieved.outcome),int(ribbon::kSpanStraight));
    QCOMPARE(relieved.protectRight,0.);QCOMPARE(relieved.protectBottom,0.); // no ribbon, no protection
    QVERIFY(SameFit(today,ribbon::Fit(bins,20,20+length,1,0,1,1,2000,400,true,refusedOnly)));
    // The same line touching the bottom of its source: refused today (outside),
    // the rectangle path under both rules.
    const auto edge=ribbon::Fit(bins,20,20+length,1,0,1,1,2000,104);
    QVERIFY(edge.unsupported);QCOMPARE(int(edge.outcome),int(ribbon::kOutside));
    for(const auto& rule:{before,refusedOnly}) {
        const auto moved=ribbon::Fit(bins,20,20+length,1,0,1,1,2000,104,true,rule);
        QVERIFY(!moved.valid&&!moved.unsupported);QCOMPARE(int(moved.outcome),int(ribbon::kSpanStraight));
    }
    // A book-page component: the same bow over 20 heights stays curved, unchanged.
    const auto bookToday=ribbon::Fit(bins,20,20+20*height,1,0,1,1,2000,400);
    QVERIFY(bookToday.valid);
    for(const auto& rule:{before,refusedOnly})
        QVERIFY(SameFit(bookToday,ribbon::Fit(bins,20,20+20*height,1,0,1,1,2000,400,true,rule)));
    // Invariant over a sweep: a rule only ever moves a component to the
    // straight outcome. Any component still fitted or refused under a rule has
    // exactly the geometry, status and protection it has today, so a line
    // that is still refused keeps its protection box. refusedOnly never
    // changes a valid fit. Lines up to K heights never change.
    int moved=0,kept=0,refusedKept=0,validKept=0;
    std::array<int,ribbon::kFitOutcomes> outcomes{};
    for(double heights:{4.,8.,15.,20.,25.,40.,90.,160.})
    for(double bow:{0.,.05,.09,.12,.2,.35,.6,1.,2.,4.})
    for(double slope:{0.,40.,600.})
    for(int sourceHeight:{400,108})
    for(const auto& rule:{before,refusedOnly}) {
        BowedBins(bins,bow*height,half,slope);
        const double u1=20+heights*height;
        const auto a=ribbon::Fit(bins,20,u1,1,0,1,1,2000,sourceHeight);
        const auto b=ribbon::Fit(bins,20,u1,1,0,1,1,2000,sourceHeight,true,rule);
        ++outcomes[a.outcome];
        if(b.outcome==ribbon::kSpanStraight) {
            QVERIFY(!b.valid&&!b.unsupported);QVERIFY(a.valid||a.unsupported);
            QVERIFY(u1-20>K*height); // only lines longer than K heights move
            // refusedOnly moves only a line that passed every shape check
            // and was refused because the padded ribbon leaves its source.
            if(rule.refusedOnly)QCOMPARE(int(a.outcome),int(ribbon::kOutside));
            ++moved;
        } else {
            QVERIFY(SameFit(a,b));++kept;refusedKept+=b.unsupported;validKept+=b.valid;
        }
        if(heights<=K)QVERIFY(SameFit(a,b));
        if(rule.refusedOnly&&a.valid)QVERIFY(SameFit(a,b));
    }
    QVERIFY(moved>10);QVERIFY(kept>10);QVERIFY(refusedKept>3);QVERIFY(validKept>3);
    // Shapes the parabola argument does not cover stay refused, with today's
    // geometry and protection, even when their least-squares bow is far under
    // the scaled bound. Lines of 90 heights (scaled bound 0.36 h).
    const auto keptRefused=[&](ribbon::FitOutcome reason,int sourceHeight) {
        const double u1=20+90*height;
        const auto a=ribbon::Fit(bins,20,u1,1,0,1,1,8000,sourceHeight);
        QCOMPARE(int(a.outcome),int(reason));QVERIFY(a.unsupported);
        QVERIFY(a.bow<ribbon::StraightBound(height,u1-20,K)/height); // the bow alone would move it
        for(const auto& rule:{refusedOnly,ribbon::StraightRule{K,true,ribbon::kStraightMaximumBow}})
            QVERIFY(SameFit(a,ribbon::Fit(bins,20,u1,1,0,1,1,8000,sourceHeight,true,rule)));
    };
    // The review's photographed page: flat for most of its length, dropping
    // half a text height over the last fifth (a gutter). Residual refusal.
    for(int i=0;i<ribbon::kBins;++i)bins[i]={};
    for(int i=0;i<ribbon::kBins;++i)for(int k=0;k<3;++k) {
        const double t=(i+(k+.5)/3)/ribbon::kBins,drop=t>.8?.5*height*((t-.8)/.2)*((t-.8)/.2):0;
        ribbon::Add(bins,t,100+drop-half);ribbon::Add(bins,t,100+drop+half);
    }
    keptRefused(ribbon::kResidual,400);
    // A strongly tapering envelope (half height 2 to 8): one rectangle height
    // cannot fit the words at its thin end. Height refusal.
    for(int i=0;i<ribbon::kBins;++i)bins[i]={};
    for(int i=0;i<ribbon::kBins;++i)for(int k=0;k<3;++k) {
        const double t=(i+(k+.5)/3)/ribbon::kBins,taper=2+6*t,center=100-4*2.*t*(1-t);
        ribbon::Add(bins,t,center-taper);ribbon::Add(bins,t,center+taper);
    }
    keptRefused(ribbon::kHeight,400);
    // The absolute cap: a 160-height line with a bow of 0.4 h is under the
    // scaled bound (0.64 h) but over the cap, so it stays refused at a source
    // edge; at 0.3 h it moves.
    const ribbon::StraightRule capped{K,true,ribbon::kStraightMaximumBow};
    for(double bow:{.3,.4}) {
        BowedBins(bins,bow*height,half);
        const double u1=20+160*height;
        const auto a=ribbon::Fit(bins,20,u1,1,0,1,1,8000,104);
        QCOMPARE(int(a.outcome),int(ribbon::kOutside));
        QVERIFY(a.bow<ribbon::StraightBound(height,u1-20,K)/height);
        const auto b=ribbon::Fit(bins,20,u1,1,0,1,1,8000,104,true,capped);
        if(bow<ribbon::kStraightMaximumBow) QCOMPARE(int(b.outcome),int(ribbon::kSpanStraight));
        else QVERIFY(SameFit(a,b));
        QCOMPARE(int(ribbon::Fit(bins,20,u1,1,0,1,1,8000,104,true,refusedOnly).outcome),int(ribbon::kSpanStraight));
    }
    // The switch reads the same for both readers.
    ribbon::StraightRule parsed{1,true,1};
    QVERIFY(PaddleFlatRibbonRule({},parsed));QCOMPARE(parsed.span,0.);
    QVERIFY(PaddleFlatRibbonRule("0",parsed));QCOMPARE(parsed.span,0.);
    QVERIFY(PaddleFlatRibbonRule("straight-refused",parsed));
    QCOMPARE(parsed.span,ribbon::kStraightSpanHeights);QVERIFY(parsed.refusedOnly);
    QCOMPARE(parsed.maximumBow,ribbon::kStraightMaximumBow);
    QVERIFY(PaddleFlatRibbonRule("straight",parsed));QVERIFY(!parsed.refusedOnly);
    for(const char* bad:{"1","Straight","straight-refused ","yes"}) {
        QVERIFY(!PaddleFlatRibbonRule(bad,parsed));QCOMPARE(parsed.span,0.);
    }
    for(auto o:{ribbon::kStraight,ribbon::kFitted,ribbon::kSteep,ribbon::kOutside})QVERIFY(outcomes[o]>0);
    // Refusal reasons are recorded by the first failed check.
    BowedBins(bins,8,5);
    QCOMPARE(int(ribbon::Fit(bins,20,420,1,0,1,1,500,200).outcome),int(ribbon::kFitted));
    QCOMPARE(int(ribbon::Fit(bins,20,420,1,0,1,1,425,200).outcome),int(ribbon::kOutside));
    BowedBins(bins,8,5,900);
    QCOMPARE(int(ribbon::Fit(bins,20,420,1,0,1,1,500,2000).outcome),int(ribbon::kSteep));
    BowedBins(bins,8,5);
    for(int i=0;i<ribbon::kBins;i+=2){bins[i].low-=6;bins[i].high-=6;} // a comb: no quadratic centre
    QCOMPARE(int(ribbon::Fit(bins,20,420,1,0,1,1,500,2000).outcome),int(ribbon::kResidual));
}
// Round 12b: both readers read OKUFLOW_UCT_TEST_FLAT_RIBBON through the same
// parser, refuse an invalid value with the same message before touching any
// model, and the CPU reader's detector post-processing (PaddleDetect) applies
// the parsed rule: a long, slightly bowed line refused today at the bottom of
// its source moves to the rectangle path, and its decoded words carry the
// line's bow (RecognizedWord::movedLineBow) while every other word carries 0.
inline void FlatSwitchBothReaders() {
    struct Environment {
        QByteArray key, old; bool present;
        Environment(const char* k, const char* value)
            : key(k), old(qgetenv(k)), present(qEnvironmentVariableIsSet(k)) { qputenv(k, value); }
        ~Environment() { if (present) qputenv(key.constData(), old); else qunsetenv(key.constData()); }
    };
    QTemporaryDir empty; QVERIFY(empty.isValid());
    {
        Environment invalid("OKUFLOW_UCT_TEST_FLAT_RIBBON", "yes");
        const auto status = CreatePaddleCpuRecognizer(empty.path())->Initialize();
        QCOMPARE(status.state, BackendState::kUnavailable);
        QCOMPARE(status.detail, QString::fromLatin1(kPaddleFlatRibbonError));
#if OKUFLOW_UCT_TRT
        const auto gpuStatus = CreateTensorRtRecognizer(empty.path())->Initialize();
        QCOMPARE(gpuStatus.state, BackendState::kUnavailable);
        QCOMPARE(gpuStatus.detail, QString::fromLatin1(kPaddleFlatRibbonError));
#endif
    }
    for (const char* valid : {"0", "straight", "straight-refused"}) {
        Environment value("OKUFLOW_UCT_TEST_FLAT_RIBBON", valid);
        const auto status = CreatePaddleCpuRecognizer(empty.path())->Initialize();
        QCOMPARE(status.state, BackendState::kUnavailable); // no models here
        QVERIFY(status.detail != QString::fromLatin1(kPaddleFlatRibbonError));
    }
    ribbon::StraightRule rule;
    QVERIFY(PaddleFlatRibbonRule("straight-refused", rule));
    constexpr int w = 1200, h = 120;
    std::vector<float> map(w * h);
    for (int y = 0; y < h; ++y) for (int x = 20; x <= 1180; ++x) {
        const double t = (x - 20) / 1160., center = 112 - 4 * 1.6 * t * (1 - t);
        if (std::abs(y - center) <= 4) map[y * w + x] = .95f;
    }
    std::atomic<bool> cancelled{};
    const auto today = PaddleDetect(map, w, h, {w, h}, cancelled, true);
    const auto moved = PaddleDetect(map, w, h, {w, h}, cancelled, true, rule);
    QCOMPARE(today.size(), 1); QCOMPARE(moved.size(), 1);
    QCOMPARE(int(today[0].ribbon.outcome), int(ribbon::kOutside)); QVERIFY(today[0].ribbon.unsupported);
    QCOMPARE(int(moved[0].ribbon.outcome), int(ribbon::kSpanStraight));
    QVERIFY(!moved[0].ribbon.valid && !moved[0].ribbon.unsupported);
    QCOMPARE(moved[0].points, today[0].points); // the same rectangle, without the ribbon flag
    QVERIFY(moved[0].ribbon.bow > .1f && moved[0].ribbon.bow < ribbon::kStraightMaximumBow);
    const QStringList dictionary{"", "a", "b", " "};
    const std::vector<PaddleToken> tokens{{1, 2, 4, .99f}, {3, 8, 8, .99f}, {2, 12, 15, .99f}};
    const auto refusedLine = PaddleDecodeTokens(tokens, 40, today[0], dictionary);
    const auto movedLine = PaddleDecodeTokens(tokens, 40, moved[0], dictionary);
    PaddleQuad plain = moved[0]; plain.ribbon = {};
    const auto plainLine = PaddleDecodeTokens(tokens, 40, plain, dictionary);
    QCOMPARE(movedLine.words.size(), 2); QCOMPARE(refusedLine.words.size(), 2); QCOMPARE(plainLine.words.size(), 2);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(refusedLine.words[i].disposition, TokenDisposition::kInvalidGeometry);
        QCOMPARE(refusedLine.words[i].movedLineBow, 0.);
        QCOMPARE(plainLine.words[i].movedLineBow, 0.);
        QCOMPARE(movedLine.words[i].disposition, TokenDisposition::kCandidate);
        QVERIFY(std::abs(movedLine.words[i].movedLineBow - moved[0].ribbon.bow) < 1e-6);
        // Apart from the provenance, a moved line's word is the plain rectangle word.
        QCOMPARE(movedLine.words[i].boundsPx, plainLine.words[i].boundsPx);
        QCOMPARE(movedLine.words[i].protectedBoundsPx, plainLine.words[i].protectedBoundsPx);
        QVERIFY(!movedLine.words[i].fromRibbon);
    }
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
