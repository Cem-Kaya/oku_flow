#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include "openzoom/common/annotation_model.hpp"

#include <QColor>
#include <QPointer>
#include <QWidget>

#include <optional>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QEvent;
class QFrame;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QSlider;
class QToolButton;
class QWheelEvent;
QT_END_NAMESPACE

namespace openzoom {

// Transparent vector-ink surface over the native camera viewport. Its compact
// child toolbar is the only opaque part of the overlay.
class AnnotationOverlay final : public QWidget {
    Q_OBJECT
public:
    AnnotationOverlay(QWidget* renderTarget, QWidget* owner);

    void SetActive(bool active);
    bool IsActive() const noexcept;
    bool HasInk() const noexcept;
    const QVector<AnnotationStroke>& Strokes() const noexcept;
    void SetViewTransform(const ViewTransform& transform);
    ViewTransform CurrentViewTransform() const noexcept;

    void SetPreferences(const QColor& color,
                        int widthPixels,
                        bool captureOnExit,
                        bool dashed = false,
                        const QString& shapeKind = QStringLiteral("rectangle"),
                        int textSizePixels = 36);
    QColor InkColor() const;
    int InkWidthPixels() const noexcept;
    bool CaptureOnExit() const noexcept;
    bool Dashed() const noexcept;
    QString ShapeKind() const;
    int TextSizePixels() const noexcept;
    AnnotationTool CurrentTool() const noexcept;
    QVector<QWidget*> FocusTargets() const;

    void ClearInk();
    bool Undo();
    bool Redo();
    bool CommitPendingText();

signals:
    void SnapshotRequested();
    void ClearRequested();
    void ExitRequested();
    void CleanPhotoRequested();
    void PreferencesChanged(const QString& colorName,
                            int widthPixels,
                            bool captureOnExit,
                            bool dashed,
                            const QString& shapeKind,
                            int textSizePixels);
    void ToolChanged(const QString& accessibleToolName);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType,
                     void* message,
                     qintptr* result) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void BuildToolbar();
    void SyncGeometryToRenderTarget();
    void PositionToolbar();
    void SetTool(AnnotationTool tool);
    void ShowToolOptions(bool visible);
    bool IsOverChrome(const QPoint& position) const;
    QRectF SelectedBoundsInView() const;
    int ScaleHandleAt(const QPointF& viewPoint) const;
    void UpdatePointerCursor(const QPointF& viewPoint);
    bool ForwardMouseToRenderTarget(QMouseEvent* event);
    bool ForwardWheelToRenderTarget(QWheelEvent* event);
    bool ForwardKeyToOwner(QKeyEvent* event);
    void EmitPreferences();
    bool MapViewPointToScene(const QPointF& viewPoint, QPointF& scenePoint) const;
    QPointF MapScenePointToView(const QPointF& scenePoint) const;
    void BeginTextEntry(const QPointF& scenePoint);
    void CancelPendingText();
    void PositionTextEditor();
    qreal CurrentSceneWidth() const;
    qreal CurrentTextHeight() const;
    qreal CurrentHitTolerance() const;
    void Announce(const QString& message);
    void UpdateToolOptions();
    void UpdateUndoButtons();
    void UpdateDirectionalUi();

    AnnotationModel model_;
    ViewTransform transform_;
    AnnotationTool tool_{AnnotationTool::kPen};
    QColor inkColor_{Qt::yellow};
    int inkWidthPixels_{8};
    int textSizePixels_{36};
    bool captureOnExit_{true};
    bool dashed_{};
    AnnotationItemKind shapeKind_{AnnotationItemKind::kRectangle};
    bool active_{};
    bool pointerDragging_{};
    bool scalingSelection_{};
    bool marqueeSelecting_{};
    bool marqueeAdditive_{};
    bool forwardingMiddleDrag_{};
    bool committingText_{};
    int activeScaleHandle_{-1};
    QPointF previousPointerScene_;
    QPointF strokeStartView_;
    QPointF marqueeStartView_;
    QPointF scaleAnchorScene_;
    QRectF marqueeRectView_;
    QRectF scaleStartBoundsView_;
    std::optional<QPointF> pendingTextScenePoint_;
    QPointer<QWidget> renderTarget_;
    QFrame* toolbar_{};
    QFrame* optionsPanel_{};
    QFrame* actionToolbar_{};
    QToolButton* penButton_{};
    QToolButton* lineButton_{};
    QToolButton* shapeButton_{};
    QToolButton* textButton_{};
    QToolButton* pointerButton_{};
    QToolButton* eraserButton_{};
    QToolButton* undoButton_{};
    QToolButton* redoButton_{};
    QToolButton* snapshotButton_{};
    QToolButton* clearButton_{};
    QToolButton* exitButton_{};
    QVector<QToolButton*> colorButtons_;
    QToolButton* solidButton_{};
    QToolButton* dashedButton_{};
    QToolButton* rectangleButton_{};
    QToolButton* ellipseButton_{};
    QLabel* optionsTitleLabel_{};
    QLabel* widthTitleLabel_{};
    QLabel* widthValueLabel_{};
    QSlider* widthSlider_{};
    QLineEdit* textInput_{};
};

} // namespace openzoom

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
