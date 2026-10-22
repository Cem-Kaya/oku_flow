#pragma once
#include "../src/common/uct_surface_geometry.hpp"
namespace uct_surface_test {
using namespace okuflow::uct;
struct Fixture { QImage image;RecognizedWord word;TextQuad expected;LineColors colors; };
inline Fixture Make(double shear,bool green=false,const QString& text=QStringLiteral("Head"),
                    int pixelSize=50,double taper=30,double sourceScale=1)
{
    uct_test::RegisterFixtureFonts();
    Fixture f;f.image=QImage(int(800*sourceScale),int(340*sourceScale),QImage::Format_ARGB32);f.image.fill(QColor(25,20,23));
    f.colors={Qt::black,green?QColor(0,245,0):QColor(245,242,244)};
    TextQuad plane{{QPointF(200,100),QPointF(540,78),QPointF(540-taper+shear,190),QPointF(200+shear,210)},true};
    for(auto& p:plane.points)p*=sourceScale;
    QPolygonF from,to;from<<QPointF(0,0)<<QPointF(340,0)<<QPointF(340,110)<<QPointF(0,110);
    for(const auto& p:plane.points)to<<p;QTransform map;QTransform::quadToQuad(from,to,map);
    QFont font(QStringLiteral("Segoe UI"));font.setPixelSize(pixelSize);
    const auto ink=QFontMetricsF(font).tightBoundingRect(text).translated(80,75);
    f.expected.points={map.map(ink.topLeft()),map.map(ink.topRight()),map.map(ink.bottomRight()),map.map(ink.bottomLeft())};f.expected.valid=true;
    {QPainter p(&f.image);p.setRenderHint(QPainter::Antialiasing);p.setTransform(map);p.setPen(Qt::NoPen);
        p.setBrush(f.colors.background);p.drawRoundedRect(QRectF(0,0,340,110),6,6);
        p.setFont(font);p.setPen(f.colors.foreground);p.drawText(QPointF(80,75),text);}
    f.word.text=text;f.word.boundsPx=f.expected.Bounds();
    f.word.quadPx=TextQuad::FromRect(f.word.boundsPx);f.word.inkFitted=f.word.paddedBounds=true;return f;
}
inline void Cpu()
{
    for(double shear:{-130.,-65.,65.,130.})for(bool green:{false,true}){
        const auto f=Make(shear,green);bool fitted=false;
        const auto q=RefineWordSurface(f.image,f.word,f.colors,&fitted);
        if(!fitted){const auto r=f.word.boundsPx;surface_geometry::Scratch scratch;
            const auto sample=[&](double x,double y,surface_geometry::Rgb& rgb){
                const int px=int(std::floor(r.x()+x)),py=int(std::floor(r.y()+y));
                if(px<0||py<0||px>=f.image.width()||py>=f.image.height())return false;
                const auto c=f.image.pixelColor(px,py);rgb={c.red(),c.green(),c.blue()};return true;};
            const auto reason=surface_geometry::Refine(r.width(),r.height(),{0,0,0},
                {f.colors.background.red(),f.colors.background.green(),f.colors.background.blue()},sample,scratch);
            qInfo()<<"surface refusal"<<shear<<green<<reason.stage<<r;
            const auto evidence=qEnvironmentVariable("OKUFLOW_UCT_SURFACE_TEST_OUTPUT");
            if(!evidence.isEmpty())f.image.save(QDir(evidence).filePath("surface-refused.png"));
        }
        QVERIFY(fitted);QVERIFY(QuadArea(q)>0);
        const auto edge=q.points[3]-q.points[0],expected=f.expected.points[3]-f.expected.points[0];
        QVERIFY(std::abs(edge.x()/edge.y()-expected.x()/expected.y())<.18);
        QVERIFY(std::abs(q.points[3].x()-q.points[0].x())>3);
        auto projective=f.word;projective.quadPx=f.expected;
        QCOMPARE(RefineWordSurface(f.image,projective,f.colors),f.expected);
        auto open=f.image;open.fill(f.colors.background);fitted=true;
        QCOMPARE(RefineWordSurface(open,f.word,f.colors,&fitted),f.word.quadPx);QVERIFY(!fitted);
        fitted=true;auto unfit=f.word;unfit.inkFitted=false;
        QCOMPARE(RefineWordSurface(f.image,unfit,f.colors,&fitted),unfit.quadPx);QVERIFY(!fitted);
        fitted=true;RefineWordSurface(f.image,f.word,{QColor(150,150,150),QColor(190,190,190)},&fitted);QVERIFY(!fitted);
        const auto cropped=f.image.copy(0,0,420,f.image.height());fitted=true;
        RefineWordSurface(cropped,f.word,f.colors,&fitted);QVERIFY(!fitted);
    }
}
inline void LabelsAndSizes()
{
    // Unrelated spellings, source sizes and surface tapers use the same rule.
    // These are generated raster evidence, not coordinates fitted to a video.
    for(const auto& text:{QStringLiteral("Zoom"),QStringLiteral("42%"),QStringLiteral("READ")})
    for(int size:{28,50})for(double shear:{-130.,65.,130.})for(double taper:{0.,70.})
    for(double sourceScale:{.75,1.,1.5}){
        const auto f=Make(shear,false,text,size,taper,sourceScale);bool fitted=false;
        const auto q=RefineWordSurface(f.image,f.word,f.colors,&fitted);
        if(!fitted){
            const auto r=f.word.boundsPx;surface_geometry::Scratch scratch;
            const auto sample=[&](double x,double y,surface_geometry::Rgb& rgb){
                const int px=int(std::floor(r.x()+x)),py=int(std::floor(r.y()+y));
                if(px<0||py<0||px>=f.image.width()||py>=f.image.height())return false;
                const auto c=f.image.pixelColor(px,py);rgb={c.red(),c.green(),c.blue()};return true;};
            const auto reason=surface_geometry::Refine(r.width(),r.height(),{0,0,0},
                {f.colors.background.red(),f.colors.background.green(),f.colors.background.blue()},sample,scratch);
            qInfo()<<"surface variant refused"<<text<<size<<shear<<taper<<sourceScale<<"stage"<<reason.stage;
        }
        QVERIFY(fitted);QVERIFY(QuadArea(q)>0);
        const auto edge=q.points[3]-q.points[0],expected=f.expected.points[3]-f.expected.points[0];
        QVERIFY(std::abs(edge.x()/edge.y()-expected.x()/expected.y())<.18);
    }
    // A tight axis box near a strongly sheared surface edge can also contain
    // dark pixels from the surrounding scene. Do not reinterpret those pixels
    // or trim them away to force a fit: keep the original geometry.
    const auto ambiguous=Make(-130,false,QStringLiteral("Zoom"),72,0);bool fitted=true;
    QCOMPARE(RefineWordSurface(ambiguous.image,ambiguous.word,ambiguous.colors,&fitted),ambiguous.word.quadPx);
    QVERIFY(!fitted);
}
inline void Paint()
{
    QWidget owner;owner.resize(400,220);owner.show();
    okuflow::UltraClearTextOverlay overlay(&owner,&owner);overlay.SetViewTransform({0,0,1,1,0,0,1,1,true});
    TextQuad near{{QPointF(30,50),QPointF(150,50),QPointF(145,90),QPointF(35,100)},true};
    TextQuad far{{QPointF(220,50),QPointF(340,50),QPointF(337,72),QPointF(223,80)},true};
    for(auto* q:{&near,&far})for(auto& p:q->points)p={p.x()/400,p.y()/220};
    overlay.SetWords({{QStringLiteral("READ"),near.Bounds(),{Qt::black,Qt::white},1,{},0,near},
        {QStringLiteral("READ"),far.Bounds(),{Qt::black,Qt::white},1,{},1,far}});
    QTRY_VERIFY(overlay.PaintedGeometryCurrent());
    okuflow::UltraClearTextOverlay::PaintedWordGeometry a,b;
    QVERIFY(overlay.PaintedWord(1,0,QStringLiteral("READ"),a));QVERIFY(a.drawn);
    QVERIFY(overlay.PaintedWord(1,1,QStringLiteral("READ"),b));QVERIFY(b.drawn);
    QVERIFY(b.box.height()<a.box.height()*.8);
    QVERIFY(a.characters.first().height()>a.characters.last().height());
    QVERIFY(b.characters.first().height()>b.characters.last().height());
    QVERIFY(!a.characters.first().isEmpty());QVERIFY(!b.characters.last().isEmpty());
    QImage image(owner.size(),QImage::Format_ARGB32);image.fill(Qt::magenta);QPainter p(&image);
    overlay.CaptureSnapshot({0,0,1,1,0,0,1,1,true},image.size()).Paint(p,image.size());p.end();
    QCOMPARE(image.pixelColor(340,110),QColor(Qt::magenta));
}
#if OKUFLOW_ENABLE_CUDA
inline void Gpu()
{
    using namespace uct_geometry_test;
    for(double shear:{-130.,-65.,65.,130.})for(bool green:{false,true})for(int size:{28,50}){
        const auto f=Make(shear,green,QStringLiteral("Head"),size);bool fitted{};const auto cpu=RefineWordSurface(f.image,f.word,f.colors,&fitted);QVERIFY(fitted);
        FixtureStream stream;GpuBuffer<unsigned char> pixels(f.image.sizeInBytes());
        GpuBuffer<gpu::SurfaceRequest> requests(1);GpuBuffer<gpu::Colors> colors(1);
        GpuBuffer<gpu::Fit> fits(1);GpuBuffer<gpu::OrientedFit> oriented(1);GpuBuffer<gpu::SurfaceFit> result(1);
        const auto r=f.word.boundsPx;const gpu::SurfaceRequest request{{r.x()-1,r.y()-1,1,0,0,1,r.width()+2,r.height()+2,0,0},0,0,0};
        const gpu::Fit fit{r.x(),r.y(),r.width(),r.height()};
        const gpu::Colors color{{0,0,0},{f.colors.background.red(),f.colors.background.green(),f.colors.background.blue()}};
        QCOMPARE(cudaMemcpyAsync(pixels.data,f.image.constBits(),f.image.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(requests.data,&request,sizeof(request),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(fits.data,&fit,sizeof(fit),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(colors.data,&color,sizeof(color),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        gpu::SurfaceFrames(pixels.data,f.image.bytesPerLine(),f.image.width(),f.image.height(),requests.data,1,
            colors.data,fits.data,oriented.data,result.data,stream.value);
        gpu::SurfaceFit q{};QCOMPARE(cudaMemcpyAsync(&q,result.data,sizeof(q),cudaMemcpyDeviceToHost,stream.value),cudaSuccess);
        QCOMPARE(cudaStreamSynchronize(stream.value),cudaSuccess);QVERIFY(q.fitted);
        for(int i=0;i<4;++i)QVERIFY(QLineF(cpu.points[i],QPointF(q.xy[i*2],q.xy[i*2+1])).length()<1e-7);
    }
}
#endif
} // namespace uct_surface_test
