/*******************************************************************************
 * Size: 16 px
 * Bpp: 1
 * Opts: --font docs_fonts_tmp_fa6.ttf --format lvgl --lv-include lvgl.h --bpp 1 --size 16 -r 0xF185 -r 0xF0C2 -r 0xF0E9 -r 0xF2DC -r 0xF72E --no-compress --force-fast-kern-format -o firmware\main\components\78__xiaozhi-fonts\src\weather_icons\weather_icons_16.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef WEATHER_ICONS_16
#define WEATHER_ICONS_16 1
#endif

#if WEATHER_ICONS_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F0C2 "" */
    0x3, 0xc0, 0x0, 0xff, 0x0, 0xf, 0xff, 0x1,
    0xff, 0xf8, 0x1f, 0xff, 0x83, 0xff, 0xf8, 0x7f,
    0xff, 0xcf, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0xff,
    0xe3, 0xff, 0xfc,

    /* U+F0E9 "" */
    0x0, 0xc0, 0x0, 0x30, 0x0, 0xff, 0xc0, 0x7f,
    0xf8, 0x3f, 0xff, 0x1f, 0xff, 0xe7, 0xff, 0xfb,
    0xff, 0xff, 0xcf, 0x3c, 0xc1, 0x86, 0x0, 0x6d,
    0x80, 0x3, 0x0, 0x0, 0xc0, 0x1, 0xb0, 0x0,
    0x7c, 0x0, 0xe, 0x0,

    /* U+F185 "" */
    0xc, 0x30, 0xe, 0x70, 0xf, 0xf0, 0xf, 0xf0,
    0xfc, 0x3f, 0xfb, 0xdf, 0x77, 0xee, 0x37, 0xec,
    0x37, 0xec, 0x77, 0xee, 0xfb, 0xdf, 0xfc, 0x3f,
    0xf, 0xf0, 0xf, 0xf0, 0xe, 0x70, 0xc, 0x30,

    /* U+F2DC "" */
    0x3, 0x0, 0x3f, 0x0, 0xfc, 0xd, 0xec, 0xf3,
    0x3d, 0xed, 0xef, 0xff, 0xc3, 0xf0, 0xf, 0xc3,
    0xff, 0xf7, 0xb7, 0xbc, 0xcf, 0x37, 0xb0, 0x3f,
    0x0, 0xfc, 0x0, 0xc0,

    /* U+F72E "" */
    0x0, 0x78, 0x0, 0x7c, 0x0, 0xc, 0x0, 0xc,
    0xff, 0xfc, 0xff, 0xf8, 0x0, 0x0, 0xff, 0xfe,
    0xff, 0xff, 0x0, 0x3, 0xfe, 0x3, 0xff, 0x1f,
    0x3, 0x1e, 0x3, 0x0, 0x1f, 0x0, 0x1e, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 320, .box_w = 20, .box_h = 14, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 35, .adv_w = 288, .box_w = 18, .box_h = 16, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 71, .adv_w = 256, .box_w = 16, .box_h = 16, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 103, .adv_w = 224, .box_w = 14, .box_h = 16, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 131, .adv_w = 256, .box_w = 16, .box_h = 16, .ofs_x = 0, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x27, 0xc3, 0x21a, 0x66c
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61634, .range_length = 1645, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 5, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t weather_icons_16 = {
#else
lv_font_t weather_icons_16 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if WEATHER_ICONS_16*/

