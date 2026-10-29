#pragma once

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
inline void CpuGeometry()
{
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
inline void GpuGeometry()
{
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
        const auto at=[&](double x,double y){return q.points[0]+u*x+v*y+warp*((x/d.width)*(y/d.height));};
        const std::array<QPointF,4> gpuPoints{at(f.u0,f.v0),at(f.u1,f.v0),at(f.u1,f.v1),at(f.u0,f.v1)};
        for(int i=0;i<4;++i) QVERIFY(QLineF(cpu.points[i],gpuPoints[i]).length()<1e-8);
    }
}
#endif
} // namespace uct_geometry_test
