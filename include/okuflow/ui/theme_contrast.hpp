#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QIcon>
#include <QImageReader>
#include <QPalette>
#include <QPixmap>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>

// Palette/scheme helpers that keep inspector text and icons readable on both
// the Windows light and dark themes. Qt follows the OS theme (there is no app
// theme setting), so every hard-coded color needs a value per scheme, chosen
// from the window surface lightness and re-applied when the scheme changes.
// Surfaces that are pinned dark by design (floating chrome, overlays, the
// display-colors popover) keep their own complete dark palette instead.
namespace okuflow::theme_contrast {

inline double RelativeLuminance(const QColor& color)
{
    const auto linear = [](int channel) {
        const double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92
                                : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.red()) + 0.7152 * linear(color.green()) +
           0.0722 * linear(color.blue());
}

inline double ContrastRatio(const QColor& first, const QColor& second)
{
    const double lighter = std::max(RelativeLuminance(first), RelativeLuminance(second));
    const double darker = std::min(RelativeLuminance(first), RelativeLuminance(second));
    return (lighter + 0.05) / (darker + 0.05);
}

inline bool IsLightSurface(const QColor& surface)
{
    return RelativeLuminance(surface) > 0.35;
}

inline bool IsLightTheme(const QPalette& palette)
{
    return IsLightSurface(palette.color(QPalette::Active, QPalette::Window));
}

// Readable disabled/placeholder grays: 7.0:1 on white, 8.2:1 on #202020.
// Disabled controls stay legible for low-vision users instead of washing out.
inline QColor DisabledTextColor(bool lightTheme)
{
    return QColor(lightTheme ? QStringLiteral("#595959")
                             : QStringLiteral("#b8b8b8"));
}

inline QColor PlaceholderTextColor(bool lightTheme)
{
    return QColor(lightTheme ? QStringLiteral("#595959")
                             : QStringLiteral("#b3b3b3"));
}

struct StatusColors {
    QColor off;
    QColor active;
    QColor warning;
};

// Status hues paired per scheme; each pair reaches at least 7:1 on its
// scheme's window surface (white / #202020).
inline StatusColors StatusColorsFor(bool lightTheme)
{
    if (lightTheme) {
        return {QColor(QStringLiteral("#595959")), QColor(QStringLiteral("#175c2e")),
                QColor(QStringLiteral("#7a4d00"))};
    }
    return {QColor(QStringLiteral("#cfcfcf")), QColor(QStringLiteral("#33d17a")),
            QColor(QStringLiteral("#f6c85f"))};
}

inline QColor WarningTextColor(bool lightTheme)
{
    return QColor(lightTheme ? QStringLiteral("#7a4d00")
                             : QStringLiteral("#ffd166"));
}

// Complete dark palette for surfaces pinned dark by design. The parentless
// display-colors popover must not inherit light-theme black text roles.
inline QPalette PinnedDarkPalette(const QPalette& base)
{
    QPalette palette = base;
    const QColor window(QStringLiteral("#161618"));
    const QColor control(QStringLiteral("#232326"));
    const QColor text(QStringLiteral("#f2f2f2"));
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, control);
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#2e2e33")));
    palette.setColor(QPalette::ToolTipBase, control);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, control);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#b8b8b8")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    const QColor disabled(QStringLiteral("#b8b8b8"));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Button,
                     QColor(QStringLiteral("#1d1d20")));
    palette.setColor(QPalette::Disabled, QPalette::Base,
                     QColor(QStringLiteral("#1d1d20")));
    return palette;
}

// Renders a bundled line icon in an explicit ink color. The Lucide resources
// mix stroke="#ffffff" and stroke="currentColor", neither of which follows
// the button surface they land on. Colored fills (the record dot) survive
// because only white/currentColor tokens are replaced. Falls back to the
// stock resource icon when no SVG image reader is available.
inline QIcon ThemedIcon(const QString& resourcePath, const QColor& color,
                        const QColor& disabledColor)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QIcon(resourcePath);
    }
    const QByteArray source = file.readAll();
    const auto render = [&source](const QColor& ink, int logicalSize) {
        QString svg = QString::fromUtf8(source);
        const QString replacement = ink.name(QColor::HexRgb);
        svg.replace(QStringLiteral("#ffffff"), replacement);
        svg.replace(QStringLiteral("#FFFFFF"), replacement);
        svg.replace(QStringLiteral("currentColor"), replacement);
        const QByteArray bytes = svg.toUtf8();
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly)) {
            return QPixmap();
        }
        QImageReader reader(&buffer, QByteArrayLiteral("svg"));
        // Render at 2x so the icon stays sharp on scaled displays.
        reader.setScaledSize(QSize(logicalSize * 2, logicalSize * 2));
        const QImage image = reader.read();
        if (image.isNull()) {
            return QPixmap();
        }
        QPixmap pixmap = QPixmap::fromImage(image);
        pixmap.setDevicePixelRatio(2.0);
        return pixmap;
    };
    QIcon icon;
    for (int size : {16, 24, 32, 48}) {
        const QPixmap normal = render(color, size);
        if (normal.isNull()) {
            return QIcon(resourcePath);
        }
        icon.addPixmap(normal, QIcon::Normal);
        icon.addPixmap(normal, QIcon::Active);
        const QPixmap disabled = render(disabledColor, size);
        if (!disabled.isNull()) {
            icon.addPixmap(disabled, QIcon::Disabled);
        }
    }
    return icon;
}

} // namespace okuflow::theme_contrast

#endif // _WIN32
