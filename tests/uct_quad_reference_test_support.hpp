#pragma once

#include "ultra_clear_text_test_support.hpp"
#include <QLineF>
#include <QElapsedTimer>
#include <QtTest>
#include <atomic>
#include <mutex>

namespace uct_quad_reference_test {
using namespace okuflow::uct;
struct Fixture {
    QImage image;
    TextQuad pixels;
    TextQuad scene;
};
inline Fixture Render(double angle, bool changed = false, bool perspective = false)
{
    uct_test::RegisterFixtureFonts();
    Fixture f;
    f.image = QImage(640, 360, QImage::Format_ARGB32);
    f.image.fill(QColor(246, 244, 236));
    const QPointF origin(170, 110), u(std::cos(angle), std::sin(angle)), v(-u.y(), u.x());
    f.pixels = {{origin, origin + u * 240, origin + u * 240 + v * 48, origin + v * 48}, true};
    if (perspective) f.pixels.points[2] -= v * 4;
    QPainterPath glyphs;
    QFont font(QStringLiteral("Segoe UI")); font.setPixelSize(44);
    glyphs.addText({}, font, changed ? QStringLiteral("Nothing") : QStringLiteral("Reading"));
    const QRectF bounds = glyphs.boundingRect();
    QPolygonF from, to;
    from << bounds.topLeft() << bounds.topRight() << bounds.bottomRight() << bounds.bottomLeft();
    for (const auto& point : f.pixels.points) to << point;
    QTransform transform;
    if (!QTransform::quadToQuad(from, to, transform)) return {};
    QPainter painter(&f.image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(transform);
    painter.fillPath(glyphs, QColor(18, 18, 22));
    f.scene = ViewQuadToScene({0,0,1,1,0,0,1,1,true},640,360,f.pixels);
    return f;
}
inline SourceStamp Stamp(int frame)
{
    SourceStamp stamp;
    stamp.sourceEpoch = stamp.geometryGeneration = 1;
    stamp.frameSeq = frame; stamp.timestampMs = frame * 33;
    stamp.viewportWidth = 640; stamp.viewportHeight = 360;
    stamp.viewTransform = {0,0,1,1,0,0,1,1,true};
    return stamp;
}
inline ProbeFrame Probe(const Fixture& f, int frame, int decimation)
{
    return MakeProbeFrame(f.image.constBits(),640,360,f.image.bytesPerLine(),decimation,Stamp(frame));
}
inline ReferencePatch Reproject(ReferencePatch patch, const ProbeFrame& target, const TextQuad& quad)
{
    const auto geometry = PlanQuadReference(target.stamp,target.Width(),target.Height(),quad);
    patch.sceneRect = geometry.sceneRect; patch.sceneQuad = geometry.sceneQuad;
    patch.valid &= geometry.sceneQuad.valid;
    return patch;
}

inline RecognizedWord FittedWord(const Fixture& fixture, double top, double bottom, bool search)
{
    const auto& q=fixture.pixels;
    const auto at=[&](double u,double v) {
        return q.points[0]*((1-u)*(1-v))+q.points[1]*(u*(1-v))+
               q.points[2]*(u*v)+q.points[3]*((1-u)*v);
    };
    RecognizedWord word;
    word.text=QStringLiteral("Reading"); word.confidence=.99;
    word.quadPx={{at(0,top),at(1,top),at(1,bottom),at(0,bottom)},true};
    word.boundsPx=word.quadPx.Bounds();word.protectedBoundsPx=fixture.pixels.Bounds();
    if(search) word.inkSearchQuadPx=fixture.pixels;
    // The fixture supplies already completed, differing ink fits. Actual
    // CPU/resident workers still prepare references and prove the pixels.
    word.inkFitted=true;
    return word;
}

// A recognition-metadata oracle only; no positive registration result is
// scripted. Every verdict comes from the production worker's actual pixels.
template<class Submit>
inline void VerifyWordProof(const Fixture& source,const Fixture& target,int control,
                            bool resident,Submit&& submit,
                            const std::function<void(const OverlayModel&,const SourceStamp&)>& surface={})
{
    struct Backend final : Recognizer {
        RecognizedWord source,target;
        bool resident{};int calls{};
        BackendStatus Initialize() override {return {BackendState::kReady,"fit-frame-oracle","",""};}
        bool WantsDeviceInput() const override {return resident;}
        RecognitionOutput Next() {
            RecognitionOutput out;out.ok=true;out.lines={{{calls++?target:source}}};return out;
        }
        RecognitionOutput Recognize(const QImage&,const std::atomic<bool>&) override {return Next();}
        RecognitionOutput RecognizeDevice(const DeviceImage&,const std::atomic<bool>&) override {return Next();}
    };
    auto backend=std::make_unique<Backend>();backend->resident=resident;
    backend->source=FittedWord(source,.15,.85,control!=1);
    backend->target=FittedWord(target,.05,.95,control!=1);
    if(control==3) {
        const auto center=backend->target.inkSearchQuadPx.Bounds().center();
        for(auto& p:backend->target.inkSearchQuadPx.points)p=center+(p-center)*2;
    }
    if(control==4) std::swap(backend->target.inkSearchQuadPx.points[1],backend->target.inkSearchQuadPx.points[2]);
    std::atomic<bool> ready{false};RecognitionResult result;
    RecognitionScheduler scheduler(std::move(backend),[&](RecognitionResult value){
        result=std::move(value);ready.store(true);
    },{});
    const auto wait=[&] {
        QElapsedTimer timer;timer.start();
        while(!ready.load()&&timer.elapsed()<5000){QCoreApplication::processEvents();QTest::qWait(1);}
        return ready.load();
    };
    QVERIFY(submit(scheduler,Stamp(1),source,CorroborationRequest{}));QVERIFY(wait());QVERIFY(result.output.ok);
    const auto selected=result;ready.store(false);
    CorroborationRequest request;request.selectedJobId=selected.jobId;request.selectedReference=selected.reference;
    request.selectedLines=selected.prepared;request.references={ReferencePatch{}}; // isolate word proof
    auto stamp=Stamp(2);if(control==5)++stamp.sourceEpoch;
    QVERIFY(submit(scheduler,stamp,target,request));QVERIFY(wait());QVERIFY(result.output.ok);
    const bool expected=control==0;
    if(control==5) QVERIFY(result.corroborationWordMatches.isEmpty());
    else {
        QCOMPARE(result.corroborationWordMatches.size(),1);
        QCOMPARE(result.corroborationWordMatches[0],QVector<bool>({expected}));
    }
    OverlayModel model;
    model.AdmitImmutable(selected,selected.stamp,selected.stamp.timestampMs,&result);
    QCOMPARE(model.SceneWords(selected.stamp.timestampMs,selected.stamp.frameSeq).size(),expected?1:0);
    QCOMPARE(model.ScreenLines(selected.stamp.timestampMs,selected.stamp.frameSeq).size(),expected?1:0);
    if(expected&&surface)surface(model,selected.stamp);
    if(resident) {
        QVERIFY(selected.reference.resident);QVERIFY(result.reference.resident);
        QVERIFY(selected.reference.luma.isNull());QVERIFY(result.reference.luma.isNull());
    }
}
} // namespace uct_quad_reference_test
