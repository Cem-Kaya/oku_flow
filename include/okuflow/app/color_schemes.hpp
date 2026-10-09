#pragma once

#include <QColor>
#include <QString>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace okuflow::color_schemes {

enum class SchemeMode {
    Duotone,
    Posterize,
    Gradient,
};

struct ColorScheme {
    QString id;
    QString name;
    QString accessibleName;
    SchemeMode mode{SchemeMode::Duotone};
    std::vector<QColor> stops;
    bool stepped{false};
    bool textColorAtHighLuma{true};
    int legacyMode{-1};
    bool effect{false};
};

using ColorLut = std::array<std::uint32_t, 256>;

const std::vector<ColorScheme>& BuiltInColorSchemes();
const ColorScheme& LegacyColorScheme(int legacyMode);
const ColorScheme* FindBuiltInColorScheme(const QString& id);
ColorScheme NormalizeColorScheme(const ColorScheme& scheme,
                                 int fallbackLegacyMode = 0);
ColorLut BuildColorLut(const ColorScheme& scheme);

std::uint32_t PackBgra(const QColor& color);
QColor UnpackBgra(std::uint32_t color);
std::uint32_t TextForegroundBgra(const ColorScheme& scheme);
std::uint32_t TextBackgroundBgra(const ColorScheme& scheme);

QString ModeToken(SchemeMode mode);
SchemeMode ModeFromToken(const QString& token);
bool SchemesEquivalent(const ColorScheme& lhs, const ColorScheme& rhs);

} // namespace okuflow::color_schemes
