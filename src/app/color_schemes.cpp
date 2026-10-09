#include "okuflow/app/color_schemes.hpp"

#include "okuflow/app/constants.hpp"

#include <algorithm>
#include <cmath>

namespace okuflow::color_schemes {

namespace {

QColor Color(const char* value)
{
    return QColor(QString::fromLatin1(value));
}

ColorScheme Pair(const char* id,
                 const char* name,
                 const char* accessibleName,
                 const char* lowColor,
                 const char* highColor,
                 bool textAtHigh,
                 int legacyMode)
{
    return {QString::fromLatin1(id),
            QString::fromLatin1(name),
            QString::fromLatin1(accessibleName),
            SchemeMode::Duotone,
            {Color(lowColor), Color(highColor)},
            false,
            textAtHigh,
            legacyMode,
            false};
}

ColorScheme Effect(const char* id,
                   const char* name,
                   const char* accessibleName,
                   SchemeMode mode,
                   std::initializer_list<const char*> stops,
                   bool stepped)
{
    ColorScheme scheme;
    scheme.id = QString::fromLatin1(id);
    scheme.name = QString::fromLatin1(name);
    scheme.accessibleName = QString::fromLatin1(accessibleName);
    scheme.mode = mode;
    scheme.stepped = stepped;
    scheme.textColorAtHighLuma = true;
    scheme.effect = true;
    for (const char* stop : stops) {
        scheme.stops.push_back(Color(stop));
    }
    return scheme;
}

int InterpolateChannel(int low, int high, double amount)
{
    return std::clamp(static_cast<int>(std::lround(
                          static_cast<double>(low) + amount * (high - low))),
                      0, 255);
}

} // namespace

const std::vector<ColorScheme>& BuiltInColorSchemes()
{
    // Stops are always dark-luma to light-luma. Legacy endpoint bytes match
    // the old CUDA kernel's truncating FloatToByte conversion, not the former
    // combo box's decorative swatches.
    static const std::vector<ColorScheme> schemes{
        Pair("normal", "Normal colors", "Normal colors", "#000000", "#ffffff", false, 0),
        Pair("inverted", "Inverted colors", "Inverted colors", "#000000", "#ffffff", true, 1),
        Pair("white-black", "White on black", "White text on black background", "#000000", "#ffffff", true, 2),
        Pair("yellow-black", "Yellow on black", "Yellow text on black background", "#000000", "#ffff00", true, 3),
        Pair("black-yellow", "Black on yellow", "Black text on yellow background", "#000000", "#ffff00", false, 4),
        Pair("cyan-black", "Cyan on black", "Cyan text on black background", "#000000", "#00ffff", true, 5),
        Pair("black-cyan", "Black on cyan", "Black text on cyan background", "#000000", "#00ffff", false, 6),
        Pair("green-black", "Green on black", "Green text on black background", "#000000", "#00ff00", true, 7),
        Pair("black-green", "Black on green", "Black text on green background", "#000000", "#00ff00", false, 8),
        Pair("amber-black", "Amber on black", "Amber text on black background", "#000000", "#ffa500", true, 9),
        Pair("black-amber", "Black on amber", "Black text on amber background", "#000000", "#ffa500", false, 10),
        Pair("white-blue", "White on blue", "White text on blue background", "#05144c", "#ffffff", true, 11),
        Pair("blue-white", "Blue on white", "Blue text on white background", "#193fff", "#ffffff", false, 12),
        Pair("yellow-blue", "Yellow on blue", "Yellow text on blue background", "#05144c", "#ffff00", true, 13),
        Pair("blue-yellow", "Blue on yellow", "Blue text on yellow background", "#193fff", "#ffff00", false, 14),
        Pair("white-dark-red", "White on dark red", "White text on dark red background", "#590505", "#ffffff", true, 15),
        Pair("dark-red-white", "Dark red on white", "Dark red text on white background", "#590505", "#ffffff", false, 16),
        Pair("magenta-black", "Magenta on black", "Magenta text on black background", "#000000", "#ff4dff", true, -1),
        Pair("black-white", "Black on white", "Black text on white background", "#000000", "#ffffff", false, -1),
        Effect("grayscale", "Grayscale", "Grayscale effect", SchemeMode::Gradient,
               {"#000000", "#ffffff"}, false),
        Effect("yellow-tint", "Yellow-tint gray", "Yellow tinted grayscale effect",
               SchemeMode::Gradient, {"#000000", "#d6b800", "#fffbd6"}, false),
        Effect("sepia", "Sepia", "Sepia effect", SchemeMode::Gradient,
               {"#1b1208", "#8b5e34", "#f2dfbd"}, false),
        Effect("posterize-6", "Posterize 6", "Six color posterize effect",
               SchemeMode::Posterize,
               {"#16161d", "#315c7c", "#3c9566", "#d0a52e", "#d8663c", "#f2e8d5"}, true),
    };
    return schemes;
}

const ColorScheme& LegacyColorScheme(int legacyMode)
{
    const int clamped = std::clamp(legacyMode, 0,
                                   app_constants::kDisplayColorModeCount - 1);
    for (const ColorScheme& scheme : BuiltInColorSchemes()) {
        if (scheme.legacyMode == clamped) {
            return scheme;
        }
    }
    return BuiltInColorSchemes().front();
}

const ColorScheme* FindBuiltInColorScheme(const QString& id)
{
    for (const ColorScheme& scheme : BuiltInColorSchemes()) {
        if (scheme.id == id) {
            return &scheme;
        }
    }
    return nullptr;
}

ColorScheme NormalizeColorScheme(const ColorScheme& scheme, int fallbackLegacyMode)
{
    if (scheme.stops.size() < 2) {
        return LegacyColorScheme(fallbackLegacyMode);
    }

    ColorScheme normalized = scheme;
    if (normalized.id.isEmpty()) {
        normalized.id = QStringLiteral("custom");
    }
    if (normalized.name.isEmpty()) {
        normalized.name = QStringLiteral("Custom colors");
    }
    if (normalized.accessibleName.isEmpty()) {
        normalized.accessibleName = normalized.name;
    }
    normalized.stops.resize(std::clamp<std::size_t>(normalized.stops.size(), 2, 8));
    for (const QColor& stop : normalized.stops) {
        if (!stop.isValid()) {
            return LegacyColorScheme(fallbackLegacyMode);
        }
    }
    if (normalized.mode == SchemeMode::Posterize) {
        normalized.stepped = true;
    }
    return normalized;
}

ColorLut BuildColorLut(const ColorScheme& requested)
{
    const ColorScheme scheme = NormalizeColorScheme(requested);
    ColorLut lut{};
    const int count = static_cast<int>(scheme.stops.size());
    for (int i = 0; i < static_cast<int>(lut.size()); ++i) {
        QColor output;
        if (scheme.stepped) {
            const int index = std::min(count - 1, i * count / 256);
            output = scheme.stops[static_cast<std::size_t>(index)];
        } else {
            const double position = static_cast<double>(i) * (count - 1) / 255.0;
            const int lowIndex = std::min(count - 1, static_cast<int>(std::floor(position)));
            const int highIndex = std::min(count - 1, lowIndex + 1);
            const double amount = position - lowIndex;
            const QColor& low = scheme.stops[static_cast<std::size_t>(lowIndex)];
            const QColor& high = scheme.stops[static_cast<std::size_t>(highIndex)];
            output = QColor(InterpolateChannel(low.red(), high.red(), amount),
                            InterpolateChannel(low.green(), high.green(), amount),
                            InterpolateChannel(low.blue(), high.blue(), amount));
        }
        lut[static_cast<std::size_t>(i)] = PackBgra(output);
    }
    return lut;
}

std::uint32_t PackBgra(const QColor& color)
{
    return static_cast<std::uint32_t>(color.blue()) |
           (static_cast<std::uint32_t>(color.green()) << 8) |
           (static_cast<std::uint32_t>(color.red()) << 16) |
           (0xffu << 24);
}

QColor UnpackBgra(std::uint32_t color)
{
    return QColor(static_cast<int>((color >> 16) & 0xffu),
                  static_cast<int>((color >> 8) & 0xffu),
                  static_cast<int>(color & 0xffu));
}

std::uint32_t TextForegroundBgra(const ColorScheme& requested)
{
    const ColorScheme scheme = NormalizeColorScheme(requested);
    return PackBgra(scheme.textColorAtHighLuma ? scheme.stops.back()
                                               : scheme.stops.front());
}

std::uint32_t TextBackgroundBgra(const ColorScheme& requested)
{
    const ColorScheme scheme = NormalizeColorScheme(requested);
    return PackBgra(scheme.textColorAtHighLuma ? scheme.stops.front()
                                               : scheme.stops.back());
}

QString ModeToken(SchemeMode mode)
{
    switch (mode) {
    case SchemeMode::Posterize: return QStringLiteral("posterize");
    case SchemeMode::Gradient: return QStringLiteral("gradient");
    case SchemeMode::Duotone: return QStringLiteral("duotone");
    }
    return QStringLiteral("duotone");
}

SchemeMode ModeFromToken(const QString& token)
{
    const QString normalized = token.trimmed().toLower();
    if (normalized == QStringLiteral("posterize")) {
        return SchemeMode::Posterize;
    }
    if (normalized == QStringLiteral("gradient")) {
        return SchemeMode::Gradient;
    }
    return SchemeMode::Duotone;
}

bool SchemesEquivalent(const ColorScheme& lhsRequested,
                       const ColorScheme& rhsRequested)
{
    const ColorScheme lhs = NormalizeColorScheme(lhsRequested);
    const ColorScheme rhs = NormalizeColorScheme(rhsRequested);
    if (lhs.mode != rhs.mode || lhs.stepped != rhs.stepped ||
        lhs.textColorAtHighLuma != rhs.textColorAtHighLuma ||
        lhs.stops.size() != rhs.stops.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.stops.size(); ++i) {
        if (lhs.stops[i].rgba() != rhs.stops[i].rgba()) {
            return false;
        }
    }
    return true;
}

} // namespace okuflow::color_schemes
