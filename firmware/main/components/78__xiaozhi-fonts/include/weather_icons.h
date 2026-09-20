#pragma once

#include <stdint.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Weather icons extracted from FontAwesome 6 Free Solid (fa-solid-900.ttf)
// Generated with lv_font_conv --bpp 1 --no-compress
// Source: https://fontawesome.com/icons

// 16px weather icons
LV_FONT_DECLARE(weather_icons_16);
extern const lv_font_t weather_icons_16;

// 48px weather icons
LV_FONT_DECLARE(weather_icons_48);
extern const lv_font_t weather_icons_48;

// Unicode constants for weather icons (FontAwesome 6 codes). Not all of
// these are baked into every size - see weather_icons_16.c/weather_icons_48.c
// for which codepoints each font actually contains.
#define FA_WEATHER_CLOUD          "\xef\x83\x82"  // U+F0C2 cloud
#define FA_WEATHER_THUNDER        "\xef\x9d\xac"  // U+F76C cloud-bolt
#define FA_WEATHER_RAIN           "\xef\x83\xa9"  // U+F0E9 umbrella (rain)
#define FA_WEATHER_SUN            "\xef\x86\x85"  // U+F185 sun
#define FA_WEATHER_SNOW           "\xef\x8b\x9c"  // U+F2DC snowflake
#define FA_WEATHER_WIND           "\xef\x9c\xae"  // U+F72E wind (16px only)
#define FA_WEATHER_PARTLY_CLOUDY  "\xef\x9b\x84"  // U+F6C4 cloud-sun (48px only)
#define FA_WEATHER_DRIZZLE        "\xef\x9c\xbd"  // U+F73D cloud-rain (48px only)
#define FA_WEATHER_HEAVY_RAIN     "\xef\x9d\x80"  // U+F740 cloud-showers-heavy (48px only)
#define FA_WEATHER_FOG            "\xef\x9d\x9f"  // U+F75F smog (48px only)

// Weather icons v2, converted from Phosphor Icons (duotone weight,
// https://phosphoricons.com) via Inkscape + Pillow (see
// tmp_icons/build_fonts.py, not checked in). Each icon is one or two
// glyphs at private-use codepoints (0xE000+): "fill"/"secondary" glyphs
// are the flat accent shape (drawn first), "detail"/"primary" glyphs are
// the outline/decoration (drawn on top). See weather_icons_v2_16.c/
// weather_icons_v2_48.c and docs/weather.md for the full mapping.

// 16px weather icons v2 (Phosphor)
LV_FONT_DECLARE(weather_icons_v2_16);
extern const lv_font_t weather_icons_v2_16;

// 48px weather icons v2 (Phosphor)
LV_FONT_DECLARE(weather_icons_v2_48);
extern const lv_font_t weather_icons_v2_48;

#define PH_WEATHER_SUN_FILL       "\xee\x80\x80"  // U+E000 sun disc (yellow)
#define PH_WEATHER_SUN_DETAIL     "\xee\x80\x81"  // U+E001 sun rays+ring (black)
#define PH_WEATHER_PARTLY_FILL    "\xee\x80\x82"  // U+E002 partly-cloudy sun peek (yellow)
#define PH_WEATHER_PARTLY_DETAIL  "\xee\x80\x83"  // U+E003 partly-cloudy outline (black)
#define PH_WEATHER_CLOUD          "\xee\x80\x84"  // U+E004 plain cloud (black)
#define PH_WEATHER_FOG            "\xee\x80\x85"  // U+E005 fog (black)
#define PH_WEATHER_UMBRELLA       "\xee\x80\x86"  // U+E006 umbrella (black) - Drizzle
#define PH_WEATHER_RAIN           "\xee\x80\x87"  // U+E007 cloud+rain streaks (black) - Rain/HeavyRain
#define PH_WEATHER_SNOW           "\xee\x80\x88"  // U+E008 cloud+snowflakes (black)
#define PH_WEATHER_THUNDER_BODY   "\xee\x80\x89"  // U+E009 cloud outline, bolt area masked out (black)
#define PH_WEATHER_THUNDER_BOLT   "\xee\x80\x8a"  // U+E00A lightning bolt only (yellow)
#define PH_WEATHER_WIND           "\xee\x80\x8b"  // U+E00B wind swirl (black)

#ifdef __cplusplus
}
#endif
