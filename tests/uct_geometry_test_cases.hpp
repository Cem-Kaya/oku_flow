#pragma once

#include "uct_small_ink_test_cases.hpp"

// Synthetic source ink, exact source mappings and production software paint.
// No model, camera, native window, or owner endpoint is involved.
namespace uct_geometry_test {
using namespace okuflow::uct;
inline TextQuad Quad(QPointF origin, double angle, bool perspective = false)
{
    const QPointF u(std::cos(angle), std::sin(angle)), v(-u.y(), u.x());
    TextQuad q;
    q.valid = true;
    q.points = {origin, origin + u * 160, origin + u * 160 + v * 40,
                origin + v * 40};
    if (perspective) q.points[2] -= v * 12;
    return q;
}
inline QImage Ink(const TextQuad& q)
{
    QImage image(320, 220, QImage::Format_ARGB32); image.fill(Qt::white);
    QPolygonF from, to;
    from << QPointF(0,0) << QPointF(160,0) << QPointF(160,40) << QPointF(0,40);
    for (const auto& p : q.points) to << p;
    QTransform transform;
    if (!QTransform::quadToQuad(from, to, transform)) return {};
    QPainter painter(&image); painter.setTransform(transform);
    for (int x = 12; x < 148; x += 16) painter.fillRect(QRect(x, 8, 6, 24), Qt::black);
    return image;
}
inline RecognizedWord Word(const TextQuad& q, QString text = QStringLiteral("Reading"))
{
    RecognizedWord word; word.text = text; word.boundsPx = word.protectedBoundsPx = q.Bounds();
    word.paddedBounds = true; word.quadPx = word.inkSearchQuadPx = q;
    return word;
}
inline void TiltedButtonColors(bool resident)
{
    uct_test::RegisterFixtureFonts();
    QVERIFY(QFontDatabase::families().contains(QStringLiteral("Segoe UI")));
    for (const double angle : {-.34, .22, .52}) for (const bool perspective : {false, true})
        for (const int touchingEdge : {0, 1, 2, 3}) for (const bool varyingInk : {false,true}) {
        const QColor ink=varyingInk?QColor(80,80,80):QColor(31,25,35), paper(244,240,246);
        QImage image(520, 400, QImage::Format_ARGB32); image.fill(ink);
        const QPointF u(std::cos(angle), std::sin(angle)), v(-u.y(), u.x()), origin(100, 135);
        const auto at = [&](double x, double y) {
            return origin + u * x + v * y - (perspective ? v * (x / 340 * y * .2) : QPointF{});
        };
        const auto quad = [&](double x, double y, double w, double h) {
            TextQuad q; q.valid = true;
            q.points = {at(x,y),at(x+w,y),at(x+w,y+h),at(x,y+h)}; return q;
        };
        QFont font(QStringLiteral("Segoe UI")); font.setPixelSize(42);
        const QFontMetricsF metrics(font);
        const double rightWidth = metrics.horizontalAdvance(QStringLiteral("Right"));
        const double handX = 16 + rightWidth + 18;
        // Use the same bilinear map as production, rather than a projective
        // painter transform, so the expected glyph bounds are independent of
        // the fit. Rasterize actual Segoe UI glyphs once in the local frame.
        QImage local(340, 86, QImage::Format_ARGB32); local.fill(ink);
        { QPainter p(&local); p.fillRect(0,0,340,64,paper); p.setFont(font); p.setPen(ink);
          p.drawText(QPointF(16,48),QStringLiteral("Right"));
          p.drawText(QPointF(handX,48),QStringLiteral("Hand"));
          p.fillRect(0,76,340,2,paper); }
        if(varyingInk) {
            const int dotLeft=int(16+metrics.horizontalAdvance(QStringLiteral("R")));
            const int dotRight=int(16+metrics.horizontalAdvance(QStringLiteral("Ri")));
            const int lastLetter=int(16+metrics.horizontalAdvance(QStringLiteral("Righ")));
            for(int y=4;y<60;++y)for(int x=16;x<16+rightWidth;++x) {
                const int value=qGray(local.pixel(x,y));
                if(y<24&&x>=dotLeft&&x<dotRight&&value<128)local.setPixelColor(x,y,Qt::black);
                if(x>=lastLetter&&value<130)local.setPixelColor(x,y,QColor(130,130,130));
            }
        }
        // Inverse of the bilinear frame for each source pixel. Top/bottom
        // edges are parallel here, so x is obtained directly along the axis.
        for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) {
            const auto d=QPointF(x+.5,y+.5)-origin;
            const double lx=QPointF::dotProduct(d,u);
            const double ly=QPointF::dotProduct(d,v)/(perspective ? 1-lx/340*.2 : 1);
            if(lx>=0 && lx<local.width() && ly>=0 && ly<local.height())
                image.setPixel(x,y,local.pixel(int(lx),int(ly)));
        }
        RecognitionOutput output; output.ok=true; output.lines.push_back({});
        const auto glyphBounds = [&](int i) {
            const int x0=int(i?handX:16),x1=int(x0+metrics.horizontalAdvance(i?QStringLiteral("Hand"):QStringLiteral("Right")));
            QRect glyph;
            for(int y=4;y<60;++y) for(int x=x0;x<=x1;++x)
                if(qGray(local.pixel(x,y))<(varyingInk?180:128)) glyph=glyph.united(QRect(x,y,1,1));
            return glyph;
        };
        for (int i=0;i<2;++i) {
            const double x=i?handX:16, width=metrics.horizontalAdvance(i?QStringLiteral("Hand"):QStringLiteral("Right"));
            auto word=Word(quad(x-3,4,width+6,56),i?QStringLiteral("Hand"):QStringLiteral("Right"));
            // Search padding reaches the identical dark surround and the
            // adjacent light rule; correct colors alone cannot isolate ink.
            const auto glyph=glyphBounds(i);
            const double left=touchingEdge==1?glyph.left():x-7;
            const double top=touchingEdge==2?glyph.top():-3;
            const double bottom=touchingEdge==3?glyph.bottom()+1:78;
            word.inkSearchQuadPx=quad(left,top,x+width+7-left,bottom-top);
            word.inkSearchPx=word.inkSearchQuadPx.Bounds();
            output.lines[0].words.push_back(word);
        }
        struct Backend final : Recognizer {
            RecognitionOutput output; bool device{};
            BackendStatus Initialize() override {return {BackendState::kReady,"button-fixture","",""};}
            bool WantsDeviceInput() const override {return device;}
            RecognitionOutput Recognize(const QImage&,const std::atomic<bool>&) override {return output;}
            RecognitionOutput RecognizeDevice(const DeviceImage&,const std::atomic<bool>&) override {return output;}
        };
        SourceStamp stamp; stamp.sourceEpoch=1;stamp.geometryGeneration=1;stamp.frameSeq=1;
        stamp.viewportWidth=image.width();stamp.viewportHeight=image.height();stamp.viewTransform={0,0,1,1,0,0,1,1,true};
        const auto prepare = [&](SharedDeviceImage device) {
            auto backend=std::make_unique<Backend>();backend->output=output;backend->device=bool(device);
            std::optional<RecognitionResult> result;std::atomic<bool> received{};
            RecognitionScheduler scheduler(std::move(backend),[&](auto r){result=std::move(r);received.store(true);},{});
            const bool submitted=device?scheduler.SubmitDevice(stamp,device):scheduler.Submit(stamp,image);
            if(!submitted)return std::optional<RecognitionResult>{};
            QElapsedTimer wait;wait.start();
            while(!received.load() && wait.elapsed()<5000) QTest::qWait(1);
            scheduler.Cancel();return result;
        };
        const auto cpu=prepare({}); QVERIFY(cpu); QVERIFY(cpu->output.ok);
        QCOMPARE(cpu->output.lines.size(),1);QCOMPARE(cpu->output.lines[0].words.size(),2);
        for(int i=0;i<2;++i) {
            const auto& word=cpu->output.lines[0].words[i];QVERIFY(word.colors);
            QCOMPARE(word.colors->foreground,ink);QCOMPARE(word.colors->background,paper);
            QCOMPARE(word.disposition,TokenDisposition::kCandidate);
            QCOMPARE(word.protectedBoundsPx,output.lines[0].words[i].protectedBoundsPx);
            if(varyingInk&&i==0) {
                QVERIFY(qGray(word.colors->foreground.rgb())>24);
                QVERIFY(130-qGray(word.colors->foreground.rgb())>24);
                if(!word.inkFitted) {
                    QCOMPARE(word.quadPx,output.lines[0].words[i].quadPx);
                    QCOMPARE(word.boundsPx,output.lines[0].words[i].boundsPx);
                    continue; // Conservative complete-word fallback, never lost dot/letter.
                }
            }
            QVERIFY2(word.inkFitted,qPrintable(QStringLiteral("angle=%1 perspective=%2 edge=%3 word=%4")
                .arg(angle).arg(perspective).arg(touchingEdge).arg(word.text)));
            // Neither the 64px button nor its external rule may become glyphs.
            QVERIFY(QuadHeight(word.quadPx)>25);QVERIFY(QuadHeight(word.quadPx)<44);
            const QRect glyph=glyphBounds(i);
            QVERIFY(!glyph.isEmpty());
            const auto expected=quad(glyph.x(),glyph.y(),glyph.width(),glyph.height());
            for(int k=0;k<4;++k) QVERIFY(QLineF(word.quadPx.points[k],expected.points[k]).length()<3);
        }
        const auto& a=cpu->output.lines[0].words[0];const auto& b=cpu->output.lines[0].words[1];
        QVERIFY(QPointF::dotProduct(b.quadPx.points[0]-a.quadPx.points[1],u)>12);
        if(!resident) continue;
#if OKUFLOW_ENABLE_CUDA
        struct Guard final : DeviceImage {
            SharedDeviceImage owned;mutable std::atomic<int> downloads{};
            explicit Guard(SharedDeviceImage p):owned(std::move(p)) {
                width=owned->width;height=owned->height;pitch=owned->pitch;device=owned->device;
                sourceEpoch=owned->sourceEpoch;frameSequence=owned->frameSequence;
            }
            const void* Data() const override{return owned->Data();}
            bool WaitReady(void* s,QString& e)const override{return owned->WaitReady(s,e);}
            bool HasResidentAnalysis()const override{return true;}
            bool PrepareResidentAnalysis(RecognitionResult& r,int d,bool p,const std::atomic<bool>& c,QString& e)const override {
                return owned->PrepareResidentAnalysis(r,d,p,c,e);
            }
            QImage Readback(const std::atomic<bool>&,QString&)const override{++downloads;return {};}
            QImage ReadProbe(const std::atomic<bool>&,QString&)const override{++downloads;return {};}
        };
        FixtureStream stream;GpuBuffer<unsigned char> pixels(image.sizeInBytes());AnalysisPool pool;
        QCOMPARE(cudaMemcpyAsync(pixels.data,image.constBits(),image.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        auto guard=std::make_shared<Guard>(pool.Capture(pixels.data,image.bytesPerLine(),image.width(),image.height(),stream.value,1,1,1));
        const auto gpu=prepare(guard);QVERIFY(gpu);QVERIFY2(gpu->output.ok,qPrintable(gpu->output.error));
        QCOMPARE(guard->downloads.load(),0);QVERIFY(gpu->reference.resident);
        QCOMPARE(gpu->output.lines[0].words.size(),2);
        for(int i=0;i<2;++i) {
            const auto& gw=gpu->output.lines[0].words[i];const auto& cw=cpu->output.lines[0].words[i];
            QVERIFY(gw.colors);QCOMPARE(gw.colors->foreground,cw.colors->foreground);QCOMPARE(gw.colors->background,cw.colors->background);
            QCOMPARE(gw.inkFitted,cw.inkFitted);QCOMPARE(gw.boundsPx,cw.boundsPx);
            for(int k=0;k<4;++k) QVERIFY(QLineF(gw.quadPx.points[k],cw.quadPx.points[k]).length()<1e-8);
        }
#endif
    }
}
inline void LooseInkPadding(bool resident)
{
    uct_test::RegisterFixtureFonts();
    const auto q=Quad({70,60},.2);
    TextQuad search=q;
    search.points[1]=q.points[0]+(q.points[1]-q.points[0])*.5;
    search.points[2]=search.points[1]+(q.points[3]-q.points[0])*.8;
    search.points[3]=q.points[0]+(q.points[3]-q.points[0])*.8;
    const auto u=(search.points[1]-search.points[0])/80;
    const auto v=(search.points[3]-search.points[0])/32;
    QImage image(320,180,QImage::Format_ARGB32);image.fill(Qt::white);
    QFont font(QStringLiteral("Segoe UI"));font.setPixelSize(40);
    QPainterPath path;path.addText({},font,QStringLiteral("held"));
    const auto ink=path.boundingRect();QVERIFY(!ink.isEmpty());
    QPolygonF from,to;from<<ink.topLeft()<<ink.topRight()<<ink.bottomRight()<<ink.bottomLeft();
    const auto at=[&](double x,double y){return search.points[0]+u*x+v*y;};
    to<<at(10,12)<<at(65,12)<<at(65,21)<<at(10,21);
    QTransform map;QVERIFY(QTransform::quadToQuad(from,to,map));
    {QPainter p(&image);p.setTransform(map);p.fillPath(path,Qt::black);}
    auto word=Word(search,QStringLiteral("held"));bool fitted{};
    const auto cpu=FitOrientedWordInk(image,word,{Qt::black,Qt::white},&fitted);
    QVERIFY2(fitted,"Readable small glyphs must fit inside loose detector padding.");
    QVERIFY(QuadHeight(cpu)>=7 && QuadHeight(cpu)<=11);
    // A short underline has enough total ink to pass the sample-count gate,
    // but must not become a complete word when the shrink fraction is lower.
    QImage noise(image.size(),image.format());noise.fill(Qt::white);
    {QPainter p(&noise);p.setTransform(QTransform(u.x(),u.y(),v.x(),v.y(),search.points[0].x(),search.points[0].y()));
     p.fillRect(QRectF(10,15,55,2),Qt::black);}
    bool noiseFitted{};
    QCOMPARE(FitOrientedWordInk(noise,word,{Qt::black,Qt::white},&noiseFitted),search);
    QVERIFY(!noiseFitted);
    // Pixel-space quad arithmetic can put a seven-pixel body a fraction
    // below seven; this must not remove readable text at the size boundary.
    for(const double bodyHeight:{7.-1e-7,6.999}) {
        auto body=search;body.points[2]=body.points[1]+v*bodyHeight;
        body.points[3]=body.points[0]+v*bodyHeight;
        RecognitionResult result;result.stamp.viewportWidth=320;result.stamp.viewportHeight=180;
        result.stamp.viewTransform={0,0,1,1,0,0,1,1,true};
        result.output.lines={{{Word(body)}}};
        const auto prepared=PrepareLines(result,7.);
        QCOMPARE(prepared.size(),1);QCOMPARE(prepared[0].words.size(),1);
        QCOMPARE(prepared[0].words[0].disposition,bodyHeight>6.9999?TokenDisposition::kCandidate:TokenDisposition::kTooSmall);
    }
    if(!resident)return;
#if OKUFLOW_ENABLE_CUDA
    for(const auto& source:{image,noise}) {
        FixtureStream stream;GpuBuffer<unsigned char> pixels(source.sizeInBytes());
        GpuBuffer<gpu::OrientedInk> desc(1);GpuBuffer<int> index(1);
        GpuBuffer<gpu::Colors> colors(1);GpuBuffer<gpu::OrientedFit> result(1);
        const gpu::OrientedInk d{search.points[0].x(),search.points[0].y(),u.x(),u.y(),v.x(),v.y(),80,32,80,0};
        const gpu::Colors c{{0,0,0},{255,255,255}};const int zero=0;
        QCOMPARE(cudaMemcpyAsync(pixels.data,source.constBits(),source.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(desc.data,&d,sizeof(d),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(colors.data,&c,sizeof(c),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(index.data,&zero,sizeof(zero),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        gpu::OrientedInkBounds(pixels.data,source.bytesPerLine(),source.width(),source.height(),desc.data,index.data,1,colors.data,result.data,stream.value);
        gpu::OrientedFit fit{};QCOMPARE(cudaMemcpyAsync(&fit,result.data,sizeof(fit),cudaMemcpyDeviceToHost,stream.value),cudaSuccess);
        QCOMPARE(cudaStreamSynchronize(stream.value),cudaSuccess);
        if(source==noise){QVERIFY(!fit.fitted);continue;}
        QVERIFY(fit.fitted);QVERIFY(fit.propagationComplete);
        const std::array<QPointF,4> points{at(fit.u0,fit.v0),at(fit.u1,fit.v0),at(fit.u1,fit.v1),at(fit.u0,fit.v1)};
        for(int i=0;i<4;++i)QVERIFY(QLineF(cpu.points[i],points[i]).length()<1e-8);
    }
#endif
}
inline void CpuGeometry()
{
    uct_small_ink_test::Run(false);
    TiltedButtonColors(false);
    LooseInkPadding(false);
    for(const double bound : {4.0,6.0}) {
        const DetailFocus focus{{400,200,80,35},bound};
        const auto region=DetailRegion({1920,1080},focus);
        const auto input=DetectorInputSize(region.size(),bound);
        QVERIFY(!region.isEmpty()); QVERIFY(region.contains(focus.regionPx));
        QVERIFY(std::max(input.width(),input.height())<=960);
        QVERIFY(double(std::max(input.width(),input.height()))/std::max(region.width(),region.height())<=bound);
        QCOMPARE(PaddleRecognitionRegions({1920,1080},focus).size(),1);
    }

    for (const double angle : {-.35, .2, .65}) {
        const auto q = Quad({80,90}, angle);
        auto word = Word(q);
        bool fitted{};
        const auto fit = FitOrientedWordInk(Ink(q), word, {Qt::black, Qt::white}, &fitted);
        QVERIFY(fitted); QVERIFY(QuadArea(fit) > 0);
        QVERIFY(std::abs(QuadAngle(fit) - angle) < 1e-8);
        QVERIFY(QuadWidth(fit) < QuadWidth(q));
        QVERIFY(QuadHeight(fit) >= 22 && QuadHeight(fit) <= 26);
        const auto scene = ViewQuadToScene({.1f,.2f,.6f,.5f,.1f,.05f,.8f,.9f,true},320,220,fit);
        const auto round = SceneQuadToView({.1f,.2f,.6f,.5f,.1f,.05f,.8f,.9f,true},320,220,scene);
        for(int i=0;i<4;++i) QVERIFY(QLineF(fit.points[i],round.points[i]).length()<1e-8);
    }
    const auto perspective = Quad({70,60}, .1, true);
    bool fitted{};
    const auto fit = FitOrientedWordInk(Ink(perspective), Word(perspective), {Qt::black,Qt::white}, &fitted);
    QVERIFY(fitted); QVERIFY(QuadIsRotated(perspective));
    QVERIFY(QLineF(fit.points[0],fit.points[3]).length() > QLineF(fit.points[1],fit.points[2]).length() + 3);
    auto invalid = perspective; std::swap(invalid.points[1],invalid.points[2]);
    QCOMPARE(QuadArea(invalid),0.0);
    QCOMPARE(QuadIntersectionArea(invalid,perspective),0.0);
}
inline void DuplicateInk()
{
    // A detector can report the close-button X as a nearly vertical letter
    // or number. Failed ink geometry must keep the original symbol; a
    // successfully fitted scalar and ordinary multi-letter text still pass.
    for(const auto text:{QStringLiteral("X"),QStringLiteral("4"),QStringLiteral("Head")})
        for(const bool fitted:{false,true}) {
            auto word=Word(Quad({80,40},1.4),text);word.inkFitted=fitted;
            RecognitionOutput output;output.lines={{{word}}};FinalizeRecognizedGeometry(output);
            QCOMPARE(output.lines[0].words[0].disposition,
                !fitted&&text.size()==1?TokenDisposition::kHighRisk:TokenDisposition::kCandidate);
        }
    // The two pre-fit CTC slash spans overlap only 65%, but fit to exactly
    // the same ink. Adjacent identical tokens must remain distinct.
    RecognitionOutput output; output.ok = true;
    auto slash = Word(TextQuad::FromRect({50,40,7,18}), QStringLiteral("/"));
    slash.inkFitted = true;
    auto adjacent = slash; adjacent.boundsPx.translate(15,0);
    adjacent.quadPx = TextQuad::FromRect(adjacent.boundsPx);
    auto duplicate = slash; duplicate.confidence = .8;
    output.lines = {{{slash,duplicate,adjacent}}};
    FinalizeRecognizedGeometry(output);
    QCOMPARE(output.lines.size(),1); QCOMPARE(output.lines[0].words.size(),2);
    QCOMPARE(output.lines[0].words[0].boundsPx, slash.boundsPx);
    QCOMPARE(output.lines[0].words[1].boundsPx, adjacent.boundsPx);
    // Mere 65% overlap before ink fitting is not a duplicate verdict.
    auto uncertain = slash; uncertain.inkFitted = false;
    auto other = uncertain; other.quadPx = other.quadPx.Translated({2.45,0}); other.boundsPx=other.quadPx.Bounds();
    output.lines = {{{uncertain,other}}}; FinalizeRecognizedGeometry(output);
    QCOMPARE(output.lines[0].words.size(),2);
}
inline void PaintGeometry()
{
    uct_test::RegisterFixtureFonts();
    QVERIFY(QFontDatabase::families().contains(QStringLiteral("Segoe UI")));
    for (const bool perspective : {false,true}) {
        QWidget owner; owner.resize(320,220);
        okuflow::UltraClearTextOverlay overlay(&owner,&owner);
        owner.show(); overlay.SetViewTransform({0,0,1,1,0,0,1,1,true});
        const auto q = Quad({70,50},.3,perspective);
        TextQuad normalized=q;
        for(auto& p:normalized.points) p=QPointF(p.x()/320,p.y()/220);
        overlay.SetWords({{QStringLiteral("READ"),normalized.Bounds(),{Qt::black,Qt::white},1,{},0,normalized}});
        QTRY_VERIFY(overlay.PaintedGeometryCurrent());
        okuflow::UltraClearTextOverlay::PaintedWordGeometry geometry;
        QVERIFY(overlay.PaintedWord(1,0,QStringLiteral("READ"),geometry)); QVERIFY(geometry.drawn);
        QImage image(320,220,QImage::Format_ARGB32); image.fill(Qt::magenta);
        QPainter painter(&image); overlay.CaptureSnapshot({0,0,1,1,0,0,1,1,true},image.size()).Paint(painter,image.size()); painter.end();
        int ink=0,paper=0;
        for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) {
            const auto pixel=image.pixelColor(x,y); ink += pixel==QColor(Qt::black); paper += pixel==QColor(Qt::white);
        }
        QVERIFY(ink>100); QVERIFY(paper>1000);
        // Bounding-box corners are not upright plates.
        QCOMPARE(image.pixelColor(225,51),QColor(Qt::magenta));
        QCOMPARE(image.pixelColor(65,130),QColor(Qt::magenta));
        QVERIFY(geometry.characters.first().center().y() < geometry.characters.last().center().y());
        // The projective height decreases towards the far end as the source does.
        if(perspective) QVERIFY(geometry.characters.first().height()>geometry.characters.last().height());
    }
    QWidget owner; owner.resize(320,220); owner.show();
    okuflow::UltraClearTextOverlay overlay(&owner,&owner); overlay.SetViewTransform({0,0,1,1,0,0,1,1,true});
    const QRectF a(.15,.25,.025,.12),b(.20,.25,.025,.12);
    overlay.SetWords({{QStringLiteral("/"),a,{Qt::black,Qt::white},1,{},0},
                      {QStringLiteral("/"),a,{Qt::black,Qt::white},1,{},1},
                      {QStringLiteral("/"),b,{Qt::black,Qt::white},1,{},2}});
    QTRY_VERIFY(overlay.PaintedGeometryCurrent());
    int drawn=0; for(int i=0;i<3;++i) { okuflow::UltraClearTextOverlay::PaintedWordGeometry g;
        QVERIFY(overlay.PaintedWord(1,i,QStringLiteral("/"),g)); drawn+=g.drawn; }
    QCOMPARE(drawn,2);
    overlay.SetWords({{QStringLiteral("READ"),{.1,.2,.4,.2},{Qt::black,Qt::white},1,{},0},
                      {QStringLiteral("OTHER"),{.1,.2,.4,.2},{Qt::black,Qt::white},2,{},0}});
    QTRY_VERIFY(overlay.PaintedGeometryCurrent());
    QImage image(320,220,QImage::Format_ARGB32); image.fill(Qt::magenta);
    const auto before=image; QPainter painter(&image);
    overlay.CaptureSnapshot({0,0,1,1,0,0,1,1,true},image.size()).Paint(painter,image.size()); painter.end();
    QCOMPARE(image,before); // ambiguous conflict leaves all original pixels
}
#if OKUFLOW_ENABLE_CUDA
inline void GpuWindingInkWorkCap()
{
    QImage image(160,160,QImage::Format_ARGB32);image.fill(Qt::white);
    const auto q=TextQuad::FromRect({10,10,128,128});
    { QPainter p(&image);p.setPen(QPen(Qt::black,1));
      p.drawLine(14,10,14,14);
      for(int y=4,index=0;y<=124;y+=4,++index) {
          p.drawLine(14,10+y,133,10+y);
          if(y<124){const int x=index%2?14:133;p.drawLine(x,10+y,x,14+y);}
      } }
    bool fitted{};
    const auto cpu=FitOrientedWordInk(image,Word(q),{Qt::black,Qt::white},&fitted);
    QVERIFY(fitted);QVERIFY(QuadArea(cpu)>0); // Exact linear CPU queue completes.
    FixtureStream stream;GpuBuffer<unsigned char> pixels(image.sizeInBytes());
    GpuBuffer<gpu::OrientedInk> desc(1);GpuBuffer<int> index(1);
    GpuBuffer<gpu::Colors> colors(1);GpuBuffer<gpu::OrientedFit> result(1);
    const gpu::OrientedInk d{10,10,1,0,0,1,128,128,128,0};
    const gpu::Colors c{{0,0,0},{255,255,255}};const int zero=0;
    QCOMPARE(cudaMemcpyAsync(pixels.data,image.constBits(),image.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
    QCOMPARE(cudaMemcpyAsync(desc.data,&d,sizeof(d),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
    QCOMPARE(cudaMemcpyAsync(colors.data,&c,sizeof(c),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
    QCOMPARE(cudaMemcpyAsync(index.data,&zero,sizeof(zero),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
    gpu::OrientedInkBounds(pixels.data,image.bytesPerLine(),image.width(),image.height(),desc.data,index.data,1,colors.data,result.data,stream.value);
    gpu::OrientedFit f{};QCOMPARE(cudaMemcpyAsync(&f,result.data,sizeof(f),cudaMemcpyDeviceToHost,stream.value),cudaSuccess);
    QCOMPARE(cudaStreamSynchronize(stream.value),cudaSuccess);
    QCOMPARE(f.propagationPasses,gpu::kOrientedInkMaxPropagationPasses);
    QVERIFY(!f.propagationComplete);QVERIFY(!f.fitted);
    QCOMPARE(f.reason,gpu::OrientedFitReason::Incomplete);
    QCOMPARE(f.u0,0.);QCOMPARE(f.u1,0.);QCOMPARE(f.v0,0.);QCOMPARE(f.v1,0.);
    qInfo()<<"Winding ink: bounded incomplete GPU fit after"<<f.propagationPasses<<"passes; CPU exact fit retained.";
}
inline void GpuGeometry()
{
    uct_small_ink_test::Run(true);
    TiltedButtonColors(true);
    LooseInkPadding(true);
    GpuWindingInkWorkCap();
    for(const double angle : {-.35,.2,.65}) for(const bool perspective:{false,true}) {
        const auto q=Quad({80,80},angle,perspective); const auto image=Ink(q); auto word=Word(q);
        bool fitted{}; const auto cpu=FitOrientedWordInk(image,word,{Qt::black,Qt::white},&fitted); QVERIFY(fitted);
        FixtureStream stream; GpuBuffer<unsigned char> pixels(image.sizeInBytes());
        GpuBuffer<gpu::OrientedInk> desc(1); GpuBuffer<int> index(1);
        GpuBuffer<gpu::Colors> colors(1); GpuBuffer<gpu::OrientedFit> result(1);
        const auto u=(q.points[1]-q.points[0])/QuadWidth(q),v=(q.points[3]-q.points[0])/QuadHeight(q);
        const auto warp=q.points[2]-q.points[1]-q.points[3]+q.points[0];
        const gpu::OrientedInk d{q.points[0].x(),q.points[0].y(),u.x(),u.y(),v.x(),v.y(),QuadWidth(q),QuadHeight(q),QuadWidth(q),0,warp.x(),warp.y()};
        const gpu::Colors c{{0,0,0},{255,255,255}}; const int zero=0;
        QCOMPARE(cudaMemcpyAsync(pixels.data,image.constBits(),image.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(desc.data,&d,sizeof(d),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(colors.data,&c,sizeof(c),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(index.data,&zero,sizeof(zero),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        gpu::OrientedInkBounds(pixels.data,image.bytesPerLine(),image.width(),image.height(),desc.data,index.data,1,colors.data,result.data,stream.value);
        gpu::OrientedFit f{};
        QCOMPARE(cudaMemcpyAsync(&f,result.data,sizeof(f),cudaMemcpyDeviceToHost,stream.value),cudaSuccess);
        QCOMPARE(cudaStreamSynchronize(stream.value),cudaSuccess); QVERIFY(f.fitted);
        QVERIFY(f.propagationComplete);QVERIFY(f.propagationPasses<=gpu::kOrientedInkMaxPropagationPasses);
        QCOMPARE(f.reason,gpu::OrientedFitReason::Fitted);
        const auto at=[&](double x,double y){return q.points[0]+u*x+v*y+warp*((x/d.width)*(y/d.height));};
        const std::array<QPointF,4> gpuPoints{at(f.u0,f.v0),at(f.u1,f.v0),at(f.u1,f.v1),at(f.u0,f.v1)};
        for(int i=0;i<4;++i) QVERIFY(QLineF(cpu.points[i],gpuPoints[i]).length()<1e-8);
    }
}
#endif
} // namespace uct_geometry_test
