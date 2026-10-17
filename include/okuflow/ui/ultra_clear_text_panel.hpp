#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QPlainTextEdit;
QT_END_NAMESPACE

namespace okuflow {

// Native, screen-reader accessible Ultra Clear Text controls: an opt-in
// switch, an always-available "show original" comparison switch, a polite
// status line and a selectable read-only pane with the accepted text.
class UltraClearTextPanel final : public QWidget {
    Q_OBJECT
public:
    explicit UltraClearTextPanel(QWidget* parent = nullptr);

    QCheckBox* enableCheckbox() const { return enableCheckbox_; }
    QCheckBox* showOriginalCheckbox() const { return showOriginalCheckbox_; }
    QLabel* statusLabel() const { return statusLabel_; }
    QPlainTextEdit* readingPane() const { return readingPane_; }

    bool IsEnabledByUser() const;
    bool IsShowingOriginal() const;
    // Status is English source text routed through the translation catalog.
    void SetStatus(const QString& sourceText, bool important = false);
    // Replaces the pane text only when it changed, preserving the reader's
    // caret and scroll position otherwise.
    void SetReadingText(const QString& text);

signals:
    void enabledChanged(bool enabled);
    void showOriginalChanged(bool showOriginal);

private:
    void UpdateDependentState();

    QCheckBox* enableCheckbox_{};
    QCheckBox* showOriginalCheckbox_{};
    QLabel* statusLabel_{};
    QPlainTextEdit* readingPane_{};
    QString statusSource_;
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
