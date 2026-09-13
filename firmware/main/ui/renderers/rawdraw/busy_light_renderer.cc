/**
 * @file busy_light_renderer.cc
 * @brief Rawdraw busy-light day-view renderer for 400x300 EPD
 */

#include "busy_light_renderer.h"

#include "i18n.h"

#include "common/presence_api.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {

namespace {

constexpr int kChipTop = Style::kStatusBarHeight;
constexpr int kChipHeight = 24;
constexpr int kGridTop = kChipTop + kChipHeight;
constexpr int kGridBottomMargin = 4;
constexpr int kLeftMargin = 34;
constexpr int kRightMargin = 6;
constexpr int kHourStart = 7;   // 7am
constexpr int kHourEnd = 19;    // 7pm
constexpr int kHourRows = kHourEnd - kHourStart;
constexpr int kWindowStartMin = kHourStart * 60;
constexpr int kWindowEndMin = kHourEnd * 60;

struct EventLayout {
    const PresenceEvent* event;
    int column;
};

// Returns local minutes-since-midnight, or -1 if RTC hasn't synced yet
// (mirrors Clock::GetTimeString()'s year<2020 sanity check).
int CurrentLocalMinutes() {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 < 2020) return -1;
    return tm_now.tm_hour * 60 + tm_now.tm_min;
}

int MinutesToY(int minutes, int grid_top, int grid_bottom) {
    const int clamped = std::max(kWindowStartMin, std::min(kWindowEndMin, minutes));
    const int grid_h = grid_bottom - grid_top;
    return grid_top + (clamped - kWindowStartMin) * grid_h / (kWindowEndMin - kWindowStartMin);
}

bool NowWithinEvent(const PresenceStatus& status, int now_minutes) {
    for (const auto& ev : status.events) {
        if (now_minutes >= ev.start_minutes && now_minutes < ev.end_minutes) return true;
    }
    return false;
}

// Below this duration, a column-1 slot is too short (a handful of pixels) to
// read as anything but an unexplained mark — route it to "+N more" instead of
// giving it dead screen space.
constexpr int kMinColumnDurationMinutes = 20;

// Greedy 2-column packing for overlapping events. Events beyond 2 concurrent
// overlaps, or too short to render meaningfully in a second column, are
// dropped into `overflow` and summarized as "+N more" — a full interval-graph
// layout isn't worth it for a single-person calendar.
void LayoutEvents(const std::vector<PresenceEvent>& events,
                  std::vector<EventLayout>* out, int* overflow_count) {
    int column_end[2] = {-1, -1};
    for (const auto& ev : events) {
        if (ev.end_minutes <= kWindowStartMin || ev.start_minutes >= kWindowEndMin) continue;
        if (column_end[0] <= ev.start_minutes) {
            out->push_back({&ev, 0});
            column_end[0] = ev.end_minutes;
        } else if (column_end[1] <= ev.start_minutes &&
                   ev.end_minutes - ev.start_minutes >= kMinColumnDurationMinutes) {
            out->push_back({&ev, 1});
            column_end[1] = ev.end_minutes;
        } else {
            (*overflow_count)++;
        }
    }
}

// Whether `item` actually overlaps something in the opposite column — only
// those events need to render at half width; an event with no concurrent
// neighbor should still use the full row width even if some other time of
// day happens to need two columns.
bool HasOppositeColumnOverlap(const std::vector<EventLayout>& laid_out, const EventLayout& item) {
    for (const auto& other : laid_out) {
        if (other.column == item.column) continue;
        if (item.event->start_minutes < other.event->end_minutes &&
            other.event->start_minutes < item.event->end_minutes) {
            return true;
        }
    }
    return false;
}

std::string TruncateToWidth(const std::string& text, const lv_font_t* font, int max_width) {
    if (!font || max_width <= 0 || text.empty()) return "";
    if (MeasureTextWidth(text.c_str(), font) <= max_width) return text;

    std::string out;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        utf8_next(&p);
        std::string next = out;
        next.append(start, p - start);
        if (MeasureTextWidth((next + "...").c_str(), font) > max_width) break;
        out = std::move(next);
    }
    return out + "...";
}

}  // namespace

BusyLightRenderer::BusyLightRenderer()
    : font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim) {
}

BusyLightRenderer::~BusyLightRenderer() {}

void BusyLightRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
}

void BusyLightRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle danger_style = theme.Style(ThemeToken::Danger);
    const PaintStyle warning_style = theme.Style(ThemeToken::Warning);
    const PaintStyle badge_style = theme.Style(ThemeToken::Badge);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, bg_style);

    const int grid_bottom = height - kGridBottomMargin;
    const int now_minutes = CurrentLocalMinutes();

    if (!current_.valid) {
        // Status chip: plain "no data" state.
        DrawStyledRect(fb, width, {0, kChipTop, width, kChipHeight}, bg_style);
        const char* no_data = i18n::Tr("暂无数据", "No data");
        const int chip_w = MeasureTextWidth(no_data, font_);
        DrawText(fb, width, (width - chip_w) / 2,
                 InkCenteredTextTopY(font_, no_data, kChipTop + kChipHeight / 2, 0),
                 no_data, font_, secondary);

        const char* empty_text = i18n::Tr("暂无日程数据", "No calendar data");
        const char* hint = i18n::Tr("长按刷新", "Long press to refresh");
        int center_y = kGridTop + (grid_bottom - kGridTop) / 2;
        int text_w = MeasureTextWidth(empty_text, font_);
        int hint_w = MeasureTextWidth(hint, font_);
        DrawText(fb, width, (width - text_w) / 2,
                 InkCenteredTextTopY(font_, empty_text, center_y - 10, 0), empty_text, font_, text);
        DrawText(fb, width, (width - hint_w) / 2,
                 InkCenteredTextTopY(font_, hint, center_y + 16, 0), hint, font_, secondary);
        needs_full_refresh_ = false;
        return;
    }

    // Status chip. Body font (16px line height) leaves real margin inside the
    // 24px chip — the title font (24px line height) was overflowing it.
    const bool busy_now = NowWithinEvent(current_, now_minutes);
    PaintStyle chip_style = bg_style;
    const char* chip_label = i18n::Tr("空闲", "Free");
    Color chip_text_color = text;
    if (current_.in_call) {
        chip_style = danger_style;
        chip_label = i18n::Tr("通话中", "In a call");
        chip_text_color = chip_style.fg;
    } else if (busy_now) {
        chip_style = warning_style;
        chip_label = i18n::Tr("忙碌", "Busy");
        chip_text_color = chip_style.fg;
    }
    DrawStyledRect(fb, width, {0, kChipTop, width, kChipHeight}, chip_style);
    const int chip_label_w = MeasureTextWidth(chip_label, font_);
    DrawText(fb, width, (width - chip_label_w) / 2,
             InkCenteredTextTopY(font_, chip_label, kChipTop + kChipHeight / 2, 0),
             chip_label, font_, chip_text_color);

    if (current_.webcam_active) {
        // Small camera pictogram (body + lens + viewfinder bump) at the chip's
        // right edge — a plain dot read as an unexplained "recording" light.
        const int cam_w = 16;
        const int cam_h = 11;
        const int cam_x = width - 14 - cam_w;
        const int cam_y = kChipTop + (kChipHeight - cam_h) / 2;
        Rect body{cam_x, cam_y, cam_w, cam_h};
        DrawRoundRect(fb, width, height, body, 2, badge_style.bg, badge_style.border, 1);
        Rect bump{cam_x + cam_w - 6, cam_y - 3, 5, 4};
        DrawRect(fb, width, bump, badge_style.bg);
        DrawRectBorder(fb, width, bump, 1, badge_style.border);
        const Point lens_c{cam_x + cam_w / 2 - 1, cam_y + cam_h / 2};
        DrawCircle(fb, width, lens_c, 3, badge_style.fg);
        DrawCircleBorder(fb, width, lens_c, 3, 1, badge_style.border);
    }
    // Explicit divider between the status chip and the grid below it — the
    // chip and grid were previously only 2px apart with no boundary, so the
    // chip's own text/badge visually bled into the grid's top row.
    DrawHLine(fb, width, kGridTop - 1, 0, width - 1, border);
    DrawHLine(fb, width, kGridTop - 2, 0, width - 1, border);

    // Hour grid: full-width lines are safe here because event blocks below
    // are drawn as solid opaque fills (see Events), which fully cover any
    // gridline underneath — no bleed-through, no text collisions.
    for (int i = 0; i <= kHourRows; ++i) {
        const int y = kGridTop + i * (grid_bottom - kGridTop) / kHourRows;
        DrawHLine(fb, width, y, kLeftMargin, width - kRightMargin, border);
        if (i < kHourRows) {
            const int mid_y = kGridTop + (i * 2 + 1) * (grid_bottom - kGridTop) / (kHourRows * 2);
            DrawHLine(fb, width, mid_y, kLeftMargin, kLeftMargin + 8, secondary);

            char label[8];
            const int hour24 = kHourStart + i;
            snprintf(label, sizeof(label), "%d", hour24 > 12 ? hour24 - 12 : hour24);
            DrawText(fb, width, 4, InkCenteredTextTopY(font_, label, y + 2, 0), label, font_, secondary);
        }
    }

    // Events (2-column overlap layout). Column width is decided per event —
    // only events that actually overlap something get squeezed to half width;
    // an unrelated event elsewhere in the day shouldn't force every row to split.
    // Blocks are solid BLACK fill with WHITE text (Outlook-style filled block,
    // not an outline) so they read as clearly "busy" at a glance and always
    // fully obscure the hour grid underneath regardless of box height.
    std::vector<EventLayout> laid_out;
    int overflow_count = 0;
    LayoutEvents(current_.events, &laid_out, &overflow_count);
    const int area_x0 = kLeftMargin + 3;
    const int area_w = width - kRightMargin - area_x0;
    for (const auto& layout : laid_out) {
        const auto& ev = *layout.event;
        const int y0 = MinutesToY(ev.start_minutes, kGridTop, grid_bottom);
        const int y1 = std::max(y0 + 3, MinutesToY(ev.end_minutes, kGridTop, grid_bottom));
        const bool split = HasOppositeColumnOverlap(laid_out, layout);
        const int col_w = split ? area_w / 2 - 2 : area_w;
        const int x0 = area_x0 + (split && layout.column == 1 ? area_w / 2 + 2 : 0);
        Rect r{x0, y0, col_w, y1 - y0};
        DrawRect(fb, width, r, BLACK);
        // Duration gate (not just pixel height) so a 15-30min event never
        // shows a title regardless of any sub-pixel rounding at this box size.
        const int duration_minutes = ev.end_minutes - ev.start_minutes;
        if (r.h >= MeasureTextHeight(font_) + 4 && duration_minutes >= 45) {
            std::string title = TruncateToWidth(ev.title.empty() ? i18n::Tr("忙碌", "Busy") : ev.title,
                                                font_, col_w - 6);
            // Must go through the ink-centering helper, not a raw y offset:
            // this font's glyph box_h/ofs_y don't map to intuitive top-left
            // coordinates (DrawText's internal glyph placement is baseline-
            // anchored), so `r.y + 2` landed the actual ink ~20px away from
            // the box. Center the ink within a virtual title-row a few px
            // below the box top so ascenders (D, l, h, t) get clearance from
            // the top edge instead of touching/appearing flattened against it.
            constexpr int kTitleRowHeight = 20;
            const int title_y = InkCenteredTextTopYInBox(font_, title.c_str(), r.y + 2, kTitleRowHeight, 0);
            DrawText(fb, width, r.x + 3, title_y, title.c_str(), font_, WHITE);
        }
    }
    if (overflow_count > 0) {
        char more_buf[24];
        snprintf(more_buf, sizeof(more_buf), i18n::Tr("+%d 更多", "+%d more"), overflow_count);
        const int more_w = MeasureTextWidth(more_buf, font_);
        // Extra clearance from the bottom-right corner: the global page frame
        // draws a rounded border on top of page content, and hugging the raw
        // edges (kRightMargin/kGridBottomMargin alone) put this label right
        // under that curve, clipping it out of view.
        constexpr int kCornerClearance = 10;
        const int more_x = width - kRightMargin - kCornerClearance - more_w;
        const int label_box_top = grid_bottom - kCornerClearance - 18;
        // Opaque background pad: this label sits over an hour gridline, and
        // black text directly on a black line read as an unreadable strike-
        // through. Punch a white pill behind it so it stays legible regardless
        // of what grid content it's sitting on top of.
        DrawRect(fb, width, {more_x - 3, label_box_top, more_w + 6, 18}, WHITE);
        const int more_y = InkCenteredTextTopYInBox(font_, more_buf, label_box_top, 18, 0);
        DrawText(fb, width, more_x, more_y, more_buf, font_, secondary);
    }

    // Now-line: solid RED (not a theme token — Accent's .fg is WHITE in this
    // theme, meant for text on a colored chip, and was invisible on the white
    // grid background), 2px thick with a small marker in the gutter.
    if (now_minutes >= kWindowStartMin && now_minutes <= kWindowEndMin) {
        const int y = MinutesToY(now_minutes, kGridTop, grid_bottom);
        DrawHLine(fb, width, y, kLeftMargin, width - kRightMargin, RED);
        DrawHLine(fb, width, y + 1, kLeftMargin, width - kRightMargin, RED);
        DrawCircle(fb, width, {kLeftMargin - 2, y}, 3, RED);
    }

    needs_full_refresh_ = false;
}

bool BusyLightRenderer::HandleInput(const ButtonEvent& event) {
    switch (event.type) {
        case ButtonEvent::kUpLongPress:
        case ButtonEvent::kDownLongPress:
        case ButtonEvent::kBootLongPress:
            presence_api_fetch_now();
            needs_full_refresh_ = true;
            return true;
        default:
            return false;
    }
}

void BusyLightRenderer::Update(const PresenceStatus& status) {
    current_ = status;
    needs_full_refresh_ = true;
}

}  // namespace rawdraw
