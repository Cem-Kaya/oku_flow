#include "okuflow/common/uct_formula_workflow.hpp"
#include <algorithm>
namespace okuflow::uct::formula {
FormulaWork Work(std::shared_ptr<Engine> engine,const QRect& region) {
    return [engine=std::move(engine),region](const SourceStamp& stamp,const QImage& pixels,SharedDeviceImage device,const std::atomic<bool>& stop) {
        FormulaObservation observation;
        if(!engine || stop.load()){observation.cancelled=stop.load();observation.error="Math model is not ready.";return observation;}
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        auto recognized=device?engine->RecognizeDevice(std::move(device),region,stop,deadline):engine->RecognizeImage(pixels,region,stop,deadline);
        observation.cancelled=recognized.cancelled || stop.load();
        if(!recognized.ok || observation.cancelled){observation.error=recognized.detail.isEmpty()?recognized.code:recognized.detail;return observation;}
        const auto layout=Typeset(recognized.latex);
        if(!layout.path){observation.error=layout.error;return observation;}
        const auto b=QRectF(recognized.boundsPx);
        const double scale=std::min(b.width()/layout.size.width(),b.height()/layout.size.height());
        const QSizeF size(layout.size.width()*scale,layout.size.height()*scale);
        const QRectF glyph(QPointF(b.center().x()-size.width()/2,b.center().y()-size.height()/2),size);
        Artifact artifact;artifact.latex=recognized.latex;artifact.path=layout.path;
        artifact.sceneRect=ViewRectToScene(stamp.viewTransform,stamp.viewportWidth,stamp.viewportHeight,b);
        artifact.glyphSceneRect=ViewRectToScene(stamp.viewTransform,stamp.viewportWidth,stamp.viewportHeight,glyph);
        artifact.foreground=recognized.foreground;artifact.background=recognized.background;
        observation.artifact=std::move(artifact);observation.ok=observation.artifact.IsValid() && !stop.load();return observation;
    };
}
}
