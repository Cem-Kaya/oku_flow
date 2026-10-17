#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include "okuflow/common/ultra_clear_text.hpp"

#include <QHash>
#include <QPointer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QPaintEvent;
QT_END_NAMESPACE

namespace okuflow {

// Click-through, non-activating window over the native camera viewport that
// draws Ultra Clear Text glyph plates. It holds only accepted, scene-registered
// words; every paint projects them through the latest presentation transform,
// so pan/zoom/resize redraw at presentation rate without recognition work.
class UltraClearTextOverlay final : public QWidget {
    Q_OBJECT
public:
    UltraClearTextOverlay(QWidget* renderTarget, QWidget* owner);

    // Called on every presentation; repaints only when geometry changed.
    void SetViewTransform(const ViewTransform& transform);
    const ViewTransform& CurrentViewTransform() const noexcept { return transform_; }

    // Scene-space words safe to show. An empty set hides the window.
    struct SceneWord {
        QString text;
        QRectF sceneRect; // offset already applied
        uct::LineColors colors;
    };
    void SetWords(QVector<SceneWord> words);
    void ClearWords();
    int WordCount() const noexcept { return words_.size(); }

    // User style: -3..3 maps onto font weight, reusing Text Clarity's
    // existing stroke-weight preference.
    void SetStrokeWeight(int weight);
    void SetExcludedWidget(QWidget* widget);

    // Words as drawn for the current transform (tests and diagnostics).
    QVector<QRectF> ProjectedWordRects() const;
    std::uint64_t PaintCount() const noexcept { return paintCount_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void SyncGeometryToRenderTarget();
    void UpdateVisibility();
    qreal AdvanceAtReference(const QString& text);

    QPointer<QWidget> renderTarget_;
    QPointer<QWidget> excludedWidget_;
    ViewTransform transform_;
    QVector<SceneWord> words_;
    QHash<QString, qreal> advanceCache_;
    int strokeWeight_{};
    std::uint64_t paintCount_{};
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
