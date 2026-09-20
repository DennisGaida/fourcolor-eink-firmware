/**
 * @file weather_renderer.cc
 * @brief Rawdraw weather page renderer for 400x300 EPD
 *
 * Layout (matches the "Current Weather" mockup):
 *
 *   [ status bar, drawn by RawDrawUiManager ]
 *   +------------------------------------------------+
 *   |  (icon) 72°           CURRENT                    |
 *   |                       WEATHER                    |
 *   |                       SATURDAY / MAY 18           |
 *   |                       HI 78°   LO 58°             |
 *   +------------------------------------------------+
 *   |  SUN            MON            TUE                |
 *   |  (icon) 68/52   (icon) 68/52   (icon) 68/52        |
 *   +------------------------------------------------+
 *   |  (icon)  TAKE AN UMBRELLA MONDAY                  |
 *   +------------------------------------------------+
 *
 * The hero "72°" number is drawn with font_hero_digits_96 - a dedicated
 * digit-only font generated from Poppins Bold (see
 * components/78__xiaozhi-fonts/src/font_hero_digits_96.c) rather than
 * repurposing font_zectrix_48_1's icon glyphs, which were designed as small
 * UI icons and looked jagged/blocky once stretched to hero-number size.
 *
 * The bottom bar is always visible: black background with yellow accents
 * when there's an active alert (rain/snow/wind/heat, or a real
 * authority-issued alert from OpenWeatherMap), plain "no alerts" text
 * otherwise.
 */

#include "weather_renderer.h"

#include "i18n.h"

#include "common/weather_api.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

// External font references (see rawdraw/font_engine.h)
extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t font_hero_digits_96;
extern const lv_font_t weather_icons_v2_48;
extern const lv_font_t weather_icons_v2_16;
extern const lv_font_t weather_icons_v2_76;  // hero icon only - larger, more breathing room

namespace rawdraw {

namespace {

// Weather icons v2: converted from Phosphor Icons (duotone weight,
// https://phosphoricons.com) - see weather_icons.h for the full codepoint
// list and components/78__xiaozhi-fonts/src/weather_icons/weather_icons_v2_*.c
// for the generated glyph data. Icons that need a color accent (Sunny,
// PartlyCloudy, Thunder) are split into two glyphs sharing the same pen
// origin: a "fill" glyph (drawn first, in YELLOW) and a "detail" glyph
// (drawn second, in BLACK, on top). Every other icon is a single flattened
// glyph drawn in whatever color the caller passes in.
constexpr const char* kIconSunFill = "\xee\x80\x80";       // U+E000 sun disc (yellow)
constexpr const char* kIconSunDetail = "\xee\x80\x81";     // U+E001 sun rays+ring (black)
constexpr const char* kIconPartlyFill = "\xee\x80\x82";    // U+E002 partly-cloudy sun peek (yellow)
constexpr const char* kIconPartlyDetail = "\xee\x80\x83";  // U+E003 partly-cloudy outline (black)
constexpr const char* kIconCloud = "\xee\x80\x84";         // U+E004 plain cloud (black)
constexpr const char* kIconFog = "\xee\x80\x85";           // U+E005 fog (black)
constexpr const char* kIconUmbrella = "\xee\x80\x86";      // U+E006 umbrella (black) - Drizzle
constexpr const char* kIconRain = "\xee\x80\x87";          // U+E007 cloud+rain streaks (black) - Rain/HeavyRain
constexpr const char* kIconSnow = "\xee\x80\x88";          // U+E008 cloud+snowflakes (black)
constexpr const char* kIconThunderBody = "\xee\x80\x89";   // U+E009 cloud outline, bolt area masked out (black)
constexpr const char* kIconThunderBolt = "\xee\x80\x8a";   // U+E00A lightning bolt only (yellow)
constexpr const char* kIconWind = "\xee\x80\x8b";          // U+E00B wind swirl (black), 16px only, alert bar

// Returns the glyph whose bounding box best represents the icon's overall
// footprint, for width/centering measurements. For two-layer composites
// this is always the larger "detail" glyph.
const char* IconGlyphFor(WeatherIcon icon) {
    switch (icon) {
        case WeatherIcon::Sunny: return kIconSunDetail;
        case WeatherIcon::PartlyCloudy: return kIconPartlyDetail;
        case WeatherIcon::Cloudy: return kIconCloud;
        case WeatherIcon::Fog: return kIconFog;
        case WeatherIcon::Drizzle: return kIconRain;
        case WeatherIcon::Rain: return kIconRain;
        case WeatherIcon::HeavyRain: return kIconUmbrella;
        case WeatherIcon::Snow: return kIconSnow;
        case WeatherIcon::Thunder: return kIconThunderBody;
        default: return kIconCloud;
    }
}

// Draws a weather icon. Sunny, PartlyCloudy and Thunder are two-layer
// composites: both glyphs share the same pen origin (they were cropped from
// identical 256x256 source canvases, so their baked-in ofs_x/ofs_y already
// line up), so the "fill" layer is simply drawn first in YELLOW and the
// "detail"/outline layer drawn second in BLACK on top - no manual offset
// tuning needed. Every other icon is a single flattened glyph in `color`.
void DrawWeatherIcon(uint8_t* fb, int width, int x, int y, WeatherIcon icon, const lv_font_t* font, Color color) {
    switch (icon) {
        case WeatherIcon::Sunny:
            DrawIcon(fb, width, x, y, kIconSunFill, font, YELLOW);
            DrawIcon(fb, width, x, y, kIconSunDetail, font, BLACK);
            return;
        case WeatherIcon::PartlyCloudy:
            DrawIcon(fb, width, x, y, kIconPartlyFill, font, YELLOW);
            DrawIcon(fb, width, x, y, kIconPartlyDetail, font, BLACK);
            return;
        case WeatherIcon::Thunder:
            DrawIcon(fb, width, x, y, kIconThunderBody, font, BLACK);
            DrawIcon(fb, width, x, y, kIconThunderBolt, font, YELLOW);
            return;
        default:
            DrawIcon(fb, width, x, y, IconGlyphFor(icon), font, color);
            return;
    }
}

struct AlertIconInfo {
    const char* glyph;
    const lv_font_t* font;
};

AlertIconInfo AlertIconFor(WeatherAlertType type) {
    switch (type) {
        case WeatherAlertType::kRain: return {kIconRain, &weather_icons_v2_16};
        case WeatherAlertType::kSnow: return {kIconSnow, &weather_icons_v2_16};
        case WeatherAlertType::kWind: return {kIconWind, &weather_icons_v2_16};
        case WeatherAlertType::kHeat: return {kIconSunDetail, &weather_icons_v2_16};
        case WeatherAlertType::kOfficial: return {kIconCloud, &weather_icons_v2_16};  // generic, no keyword-matched icon
        default: return {nullptr, &weather_icons_v2_16};
    }
}

// Alert bar message, split into a plain-white prefix and a yellow-accented
// suffix (the day it applies to) so the two can be drawn in different
// colors, matching the mockup's "TAKE AN UMBRELLA <MONDAY>" styling.
struct AlertMessageParts {
    std::string prefix;
    std::string accent;  // day label, or empty if nothing to accent
};

AlertMessageParts BuildAlertMessage(const WeatherAlert& alert) {
    if (alert.type == WeatherAlertType::kOfficial && !alert.event_text.empty()) {
        std::string text = alert.event_text;
        for (char& c : text) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        return {text, ""};
    }
    const std::string day = alert.day_label.empty() ? "TODAY" : alert.day_label;
    switch (alert.type) {
        case WeatherAlertType::kRain: return {"TAKE AN UMBRELLA ", day};
        case WeatherAlertType::kSnow: return {"SNOW EXPECTED ", day};
        case WeatherAlertType::kWind: return {"HIGH WINDS ", day};
        case WeatherAlertType::kHeat: return {"HEAT WARNING ", day};
        default: return {"NO WEATHER ALERTS TODAY", ""};
    }
}

// ============================================================
// Big hero number rendering using font_hero_digits_96 - a dedicated
// digit-only font (plain ASCII '0'-'9' and '-', no glyph translation
// needed unlike the old icon-font workaround).
// ============================================================

// Draws `text` (digits, optionally prefixed with '-') at (x, y) using the
// big hero digit font. Returns the total width consumed.
int DrawBigNumber(uint8_t* fb, int width, int x, int y, const std::string& text, Color color) {
    DrawText(fb, width, x, y, text.c_str(), &font_hero_digits_96, color);
    return MeasureTextWidth(text.c_str(), &font_hero_digits_96);
}

}  // namespace

WeatherRenderer::WeatherRenderer()
    : font_(&SourceHanSansSC_Regular_slim), title_font_(&SourceHanSansSC_Medium_slim) {
}

WeatherRenderer::~WeatherRenderer() {}

void WeatherRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    has_data_ = false;
    needs_full_refresh_ = true;
}

void WeatherRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, bg_style);

    if (!has_data_) {
        const char* empty_text = i18n::Tr(i18n::StringId::kNoWeatherData);
        const char* hint = i18n::Tr(i18n::StringId::kLongPressToRefresh);
        int text_w = MeasureTextWidth(empty_text, font_);
        int hint_w = MeasureTextWidth(hint, font_);
        int center_y = Style::kStatusBarHeight + (height - Style::kStatusBarHeight) / 2;
        DrawText(fb, width, (width - text_w) / 2, InkCenteredTextTopY(font_, empty_text, center_y - 10),
                 empty_text, font_, text);
        DrawText(fb, width, (width - hint_w) / 2, InkCenteredTextTopY(font_, hint, center_y + 16),
                 hint, font_, secondary);
        needs_full_refresh_ = false;
        return;
    }

    // ------------------------------------------------------------
    // Section layout (see file header for the ASCII sketch)
    // ------------------------------------------------------------
    constexpr int kHeroTop = Style::kStatusBarHeight + 4;    // 32
    constexpr int kHeroBottom = 130;  // nominal box used only to compute the vertical center
    constexpr int kForecastBottom = 250;
    constexpr int kAlertTop = 262;
    constexpr int kAlertBottom = 296;

    constexpr int kLeftColumnX = 16;
    constexpr int kRightColumnMinX = 222;
    constexpr int kRightColumnRight = 388;

    // --- Hero: icon + big temperature side-by-side (left), heading + date +
    // hi/lo (right) - both blocks vertically centered on the same horizontal
    // band so the icon/number don't look top-heavy relative to the heading.
    const int hero_box_center_y = (kHeroTop + kHeroBottom) / 2;

    const WeatherIcon hero_icon = WeatherIconForCode(current_data_.condition_code);
    const int hero_icon_h = MeasureTextHeight(&weather_icons_v2_76);
    const int hero_icon_y = hero_box_center_y - hero_icon_h / 2;
    DrawWeatherIcon(fb, width, kLeftColumnX, hero_icon_y, hero_icon, &weather_icons_v2_76, text);
    const int hero_icon_w = MeasureTextWidth(IconGlyphFor(hero_icon), &weather_icons_v2_76);

    char temp_buf[8];
    snprintf(temp_buf, sizeof(temp_buf), "%d", static_cast<int>(current_data_.temp));
    const int number_x = kLeftColumnX + hero_icon_w + 28;
    const int digit_y = InkCenteredTextTopY(&font_hero_digits_96, temp_buf, hero_box_center_y);
    const int number_w = DrawBigNumber(fb, width, number_x, digit_y, temp_buf, text);
    // Degree mark: a small ring at the top-right of the number (no glyph for
    // "°" in the big-digit font).
    DrawCircleBorder(fb, width, {number_x + number_w + 3, digit_y + 6}, 5, 2, text);

    const int right_column_x = std::max(kRightColumnMinX, number_x + number_w + 26);

    // Two-line "CURRENT" / "WEATHER" heading (condition text, e.g. "scattered
    // clouds", is intentionally not shown - it just duplicates the icon).
    // The whole right-side block (heading + date + hi/lo) is vertically
    // centered on hero_box_center_y, same as the icon/number on the left.
    const int title_line_h = MeasureTextHeight(title_font_);
    const int date_line_h = MeasureTextHeight(font_);
    const int right_block_h = 3 * title_line_h + date_line_h + 12;
    const int heading_y = hero_box_center_y - right_block_h / 2;
    DrawText(fb, width, right_column_x, heading_y, "CURRENT", title_font_, text);
    DrawText(fb, width, right_column_x, heading_y + title_line_h, "WEATHER", title_font_, RED);

    const int date_y = heading_y + 2 * title_line_h + 4;
    DrawText(fb, width, right_column_x, date_y, current_data_.date_label.c_str(), font_, text);

    const int hilo_y = date_y + date_line_h + 8;
    char hi_buf[16];
    char lo_buf[16];
    snprintf(hi_buf, sizeof(hi_buf), "HI %d\xc2\xb0", static_cast<int>(current_data_.temp_max_today));
    snprintf(lo_buf, sizeof(lo_buf), "LO %d\xc2\xb0", static_cast<int>(current_data_.temp_min_today));
    DrawText(fb, width, right_column_x, hilo_y, hi_buf, title_font_, RED);
    const int hi_w = MeasureTextWidth(hi_buf, title_font_);
    DrawText(fb, width, right_column_x + hi_w + 16, hilo_y, lo_buf, title_font_, text);

    // Divider sits below whichever block (icon or heading/date/hi-lo) runs
    // lower, with enough clearance that it doesn't crowd the HI/LO line.
    const int divider_y = std::max(hero_icon_y + hero_icon_h, hilo_y + title_line_h) + 14;
    DrawHLine(fb, width, divider_y, kLeftColumnX, kRightColumnRight, border);

    // --- 3-day forecast row: day (bold) + icon on the left, hi/lo stacked on
    // the right, per card ----------------------------------------------------
    const int forecast_top = divider_y + 8;
    constexpr int kForecastMargin = 18;
    constexpr int kForecastGap = 8;
    const int forecast_card_w = (width - 2 * kForecastMargin - 2 * kForecastGap) / 3;
    const int forecast_h = kForecastBottom - forecast_top;

    for (size_t i = 0; i < current_data_.forecast.size() && i < 3; ++i) {
        const auto& day = current_data_.forecast[i];
        const int card_x = kForecastMargin + static_cast<int>(i) * (forecast_card_w + kForecastGap);
        const Rect card{card_x, forecast_top, forecast_card_w, forecast_h};

        constexpr int kCardPad = 8;
        std::string label = day.weekday_label;
        for (char& c : label) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        DrawText(fb, width, card.x + kCardPad,
                 InkCenteredTextTopY(title_font_, label.c_str(), card.y + card.h / 2 - 22), label.c_str(),
                 title_font_, secondary);

        const WeatherIcon day_icon = WeatherIconForCode(day.condition_code);
        const int icon_center_y = card.y + card.h / 2 + 14;
        DrawWeatherIcon(fb, width, card.x + kCardPad,
                         InkCenteredTextTopY(&weather_icons_v2_48, IconGlyphFor(day_icon), icon_center_y), day_icon,
                         &weather_icons_v2_48, text);

        char hi_buf2[16];
        char lo_buf2[16];
        snprintf(hi_buf2, sizeof(hi_buf2), "%d\xc2\xb0", static_cast<int>(day.temp_max));
        snprintf(lo_buf2, sizeof(lo_buf2), "%d\xc2\xb0", static_cast<int>(day.temp_min));
        const int hi2_w = MeasureTextWidth(hi_buf2, title_font_);
        const int lo2_w = MeasureTextWidth(lo_buf2, font_);
        const int hi2_h = MeasureTextHeight(title_font_);
        const int lo2_h = MeasureTextHeight(font_);
        const int block_h = hi2_h + 2 + lo2_h;
        const int block_top = card.y + (card.h - block_h) / 2;
        DrawText(fb, width, card.x + card.w - kCardPad - hi2_w, block_top, hi_buf2, title_font_, RED);
        DrawText(fb, width, card.x + card.w - kCardPad - lo2_w, block_top + hi2_h + 2, lo_buf2, font_, text);

        if (i > 0) {
            DrawVLine(fb, width, card.x - kForecastGap / 2, card.y + 4, card.y + card.h - 4, border);
        }
    }

    // --- Bottom alert bar ----------------------------------------------------
    // Black background with yellow accents when active (badge behind the
    // icon, and the day-word suffix); plain/neutral when there's nothing to
    // warn about. Kept slim (bar height ~34px) so it doesn't dominate the
    // footer with empty black space above/below the text.
    const Rect alert_bar{kForecastMargin, kAlertTop, width - 2 * kForecastMargin, kAlertBottom - kAlertTop};
    const bool alert_active = current_data_.alert.type != WeatherAlertType::kNone;
    const Color alert_bg = alert_active ? BLACK : WHITE;
    const Color alert_fg = alert_active ? WHITE : text;
    DrawRoundRect(fb, width, height, alert_bar, Style::kBorderRadiusMD, alert_bg, BLACK, 1);

    const AlertIconInfo alert_icon = AlertIconFor(current_data_.alert.type);
    const AlertMessageParts alert_msg = BuildAlertMessage(current_data_.alert);
    const int bar_center_y = alert_bar.y + alert_bar.h / 2;
    int text_x = alert_bar.x + 12;
    if (alert_icon.glyph && alert_active) {
        // Badge radius is sized to the bar height (minus a little padding) -
        // filled circles on this display have no anti-aliasing, so a larger
        // radius relative to the bar keeps the pixel-step edges from
        // dominating the shape.
        const int kBadgeRadius = std::min(14, alert_bar.h / 2 - 2);
        const int badge_cx = text_x + kBadgeRadius;
        DrawCircle(fb, width, {badge_cx, bar_center_y}, kBadgeRadius, YELLOW);
        DrawIcon(fb, width, badge_cx - MeasureTextWidth(alert_icon.glyph, alert_icon.font) / 2,
                 InkCenteredTextTopY(alert_icon.font, alert_icon.glyph, bar_center_y), alert_icon.glyph,
                 alert_icon.font, BLACK);
        text_x += kBadgeRadius * 2 + 10;
    }
    DrawText(fb, width, text_x, InkCenteredTextTopY(font_, alert_msg.prefix.c_str(), bar_center_y),
             alert_msg.prefix.c_str(), font_, alert_fg);
    if (!alert_msg.accent.empty()) {
        const int prefix_w = MeasureTextWidth(alert_msg.prefix.c_str(), font_);
        DrawText(fb, width, text_x + prefix_w, InkCenteredTextTopY(font_, alert_msg.accent.c_str(), bar_center_y),
                 alert_msg.accent.c_str(), font_, YELLOW);
    }

    needs_full_refresh_ = false;
}

bool WeatherRenderer::HandleInput(const ButtonEvent& event) {
    switch (event.type) {
        case ButtonEvent::kUpLongPress:
        case ButtonEvent::kDownLongPress:
        case ButtonEvent::kBootLongPress:
            weather_api_fetch_now();
            return true;
        default:
            return false;
    }
}

void WeatherRenderer::Update(const WeatherData& data) {
    current_data_ = data;
    has_data_ = true;
    needs_full_refresh_ = true;
}

}  // namespace rawdraw
