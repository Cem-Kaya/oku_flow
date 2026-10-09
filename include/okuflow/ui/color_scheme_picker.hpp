#pragma once

#include "okuflow/app/color_schemes.hpp"

#include <QWidget>

#include <vector>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QGridLayout;
class QPushButton;
class QSpinBox;
class QToolButton;
QT_END_NAMESPACE

namespace okuflow {

class ColorSchemePicker final : public QWidget {
    Q_OBJECT
public:
    explicit ColorSchemePicker(QWidget* parent = nullptr);
    ~ColorSchemePicker() override;

    const color_schemes::ColorScheme& currentScheme() const { return currentScheme_; }
    const color_schemes::ColorScheme& customScheme() const { return customScheme_; }
    void setCurrentScheme(const color_schemes::ColorScheme& scheme);
    void setCustomScheme(const color_schemes::ColorScheme& scheme);
    bool hasCustomScheme() const { return customScheme_.stops.size() >= 2; }

signals:
    void schemeChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void BuildPopup();
    void ShowPopup();
    void HidePopup(bool restoreFocus = true);
    void SelectScheme(const color_schemes::ColorScheme& scheme);
    void RefreshSelection();
    void RefreshCustomEditor();
    void RefreshCustomTile();
    void ApplyCustomScheme();
    void Announce(const QString& message);

    color_schemes::ColorScheme currentScheme_;
    color_schemes::ColorScheme customScheme_;
    QPushButton* trigger_{};
    QWidget* popup_{};
    std::vector<QToolButton*> schemeTiles_;
    std::vector<QToolButton*> colorWells_;
    QToolButton* customTile_{};
    QComboBox* customModeCombo_{};
    QSpinBox* stopCountSpin_{};
    QCheckBox* steppedCheck_{};
    QWidget* customPreview_{};
    QPushButton* applyCustomButton_{};
    std::vector<QColor> editorStops_;
};

} // namespace okuflow
