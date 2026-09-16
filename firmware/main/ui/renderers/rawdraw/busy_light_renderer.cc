/**
 * @file busy_light_renderer.cc
 * @brief Rawdraw busy-light renderer for 400x300 EPD
 *
 * Two faces, toggled by a BOOT click (see BusyLightRenderer::View):
 *  - kDefault: hallway glance — one word, one human line, camera state, and a
 *    thin day rail so "what does the rest of the day look like" is answerable
 *    without reading anything.
 *  - kDetail: your own view — the full day grid with per-event importance
 *    carried by the block's fill (outline/yellow-rule/solid-red).
 *
 * Layout follows the "Busy Light Display" design doc (rounds 4a/4b): a
 * color-coded status band, importance carried by fill (not just a label),
 * and a hallway rail that answers "what does later look like" for free.
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

constexpr int kHeaderTop = Style::kStatusBarHeight;
constexpr int kHeaderHeight = 26;
constexpr int kGridTop = kHeaderTop + kHeaderHeight;
constexpr int kGridBottomMargin = 4;
constexpr int kLeftMargin = 34;
constexpr int kRightMargin = 6;
constexpr int kHourStart = 8;   // 8am
constexpr int kHourEnd = 18;    // 6pm
constexpr int kHourRows = kHourEnd - kHourStart;
constexpr int kWindowStartMin = kHourStart * 60;
constexpr int kWindowEndMin = kHourEnd * 60;

struct EventLayout {
    const PresenceEvent* event;
    int column;
};

// TODO(busy-light): flip to false once there's a reason to see the page at
// real wall-clock time during dev sessions (calendar data itself now comes
// from a real fetch — see presence_api_init in application.cc — this flag
// is only about which time-of-day the page renders against).
// The RTC syncs correctly, but real wall-clock time is rarely inside
// business hours during a design/mocking session, which left the zoomed
// detail view's window and now-marker with nothing interesting to show.
// Pretending it's always mid-morning keeps the mock day's events centered
// in view regardless of when you actually flash and look at the device.
constexpr bool kMockCurrentTime = true;
constexpr int kMockCurrentTimeMinutes = 10 * 60 + 30;  // 10:30am

// Returns local minutes-since-midnight, or -1 if RTC hasn't synced yet
// (mirrors Clock::GetTimeString()'s year<2020 sanity check).
int CurrentLocalMinutes() {
    if (kMockCurrentTime) return kMockCurrentTimeMinutes;
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 < 2020) return -1;
    return tm_now.tm_hour * 60 + tm_now.tm_min;
}

int MinutesToY(int minutes, int window_start, int window_end, int grid_top, int grid_bottom) {
    const int clamped = std::max(window_start, std::min(window_end, minutes));
    const int grid_h = grid_bottom - grid_top;
    return grid_top + (clamped - window_start) * grid_h / (window_end - window_start);
}

// Debug/demo cycle for the DOWN button — see BusyLightRenderer::HandleInput.
// Some entries are intentionally redundant (e.g. "cam off" and "presentation
// off" both land on cam=false/presenting=false) because the point is to step
// through the exact list requested, not to dedupe visually-similar states.
struct DebugAvState {
    bool cam;
    bool presenting;
};
constexpr DebugAvState kDebugAvStates[] = {
    {true, false},   // cam on
    {false, false},  // cam off
    {false, true},   // presentation on
    {false, false},  // presentation off
    {true, true},    // cam on & presentation on
    {false, true},   // cam off & presentation on
};
constexpr int kDebugAvStateCount = sizeof(kDebugAvStates) / sizeof(kDebugAvStates[0]);

PresenceEventTier DebugTierToPresenceTier(BusyLightRenderer::DebugTier tier) {
    switch (tier) {
        case BusyLightRenderer::DebugTier::kLeadership: return PresenceEventTier::kLeadership;
        case BusyLightRenderer::DebugTier::kCustomer: return PresenceEventTier::kCustomer;
        case BusyLightRenderer::DebugTier::kInternal:
        case BusyLightRenderer::DebugTier::kFree:
        default: return PresenceEventTier::kInternal;
    }
}

bool DebugTierIsBusy(BusyLightRenderer::DebugTier tier) {
    return tier != BusyLightRenderer::DebugTier::kFree;
}

// Whether this tier is important enough to color the top status band red.
bool IsElevated(PresenceEventTier tier) {
    return tier != PresenceEventTier::kInternal && tier != PresenceEventTier::kSolo;
}

// Effective header status after folding in the debug override. Shared by
// both faces' headers so "ad-hoc call" (free but camera/presenting active —
// see the design doc's own `adhoc` rule) is computed identically everywhere.
struct EffectiveStatus {
    bool busy;
    PresenceEventTier tier;   // meaningful only if busy
    bool band_elevated;       // top band / swatch should read as elevated (red)
    bool is_adhoc;
};

EffectiveStatus ComputeEffectiveStatus(BusyLightRenderer::DebugTier debug_tier, bool cam, bool presenting) {
    EffectiveStatus s;
    const bool base_busy = DebugTierIsBusy(debug_tier);
    // Ad-hoc call: nothing on the calendar, but the camera or a screen-share
    // is live — the design doc treats that as a customer-severity call
    // ("Ad-hoc call", never silently reads as FREE) rather than pretending
    // nobody's in the room.
    s.is_adhoc = !base_busy && (cam || presenting);
    s.busy = base_busy || s.is_adhoc;
    s.tier = s.is_adhoc ? PresenceEventTier::kCustomer : DebugTierToPresenceTier(debug_tier);
    s.band_elevated = presenting || (s.busy && IsElevated(s.tier));
    return s;
}

const PresenceEvent* ActiveEvent(const PresenceStatus& status, int now_minutes) {
    const PresenceEvent* best = nullptr;
    for (const auto& ev : status.events) {
        if (now_minutes < ev.start_minutes || now_minutes >= ev.end_minutes) continue;
        // Prefer the highest-severity concurrent event (customer > leadership
        // > internal) so the headline word/tier never understates the room.
        if (!best || static_cast<int>(ev.tier) > static_cast<int>(best->tier)) best = &ev;
    }
    return best;
}

const PresenceEvent* NextEvent(const PresenceStatus& status, int now_minutes) {
    const PresenceEvent* next = nullptr;
    for (const auto& ev : status.events) {
        if (ev.start_minutes <= now_minutes) continue;
        if (!next || ev.start_minutes < next->start_minutes) next = &ev;
    }
    return next;
}

// Detail-grid fill / header-swatch color: internal reads as "nothing special"
// (white/outline), leadership as yellow, customer as red. Matches the design
// doc's per-tier "bg" table exactly (Free and Internal meeting both = white).
Color TierColor(PresenceEventTier tier) {
    switch (tier) {
        case PresenceEventTier::kCustomer: return RED;
        case PresenceEventTier::kLeadership: return YELLOW;
        case PresenceEventTier::kInternal:
        default: return WHITE;
    }
}

// Hallway-rail color: unlike the grid/header, the rail only distinguishes
// "customer" (red — the one thing worth flagging from across the hallway)
// from "anything else busy" (black). Matches the design doc's rail markup,
// which never uses yellow.
Color RailColor(PresenceEventTier tier) {
    return tier == PresenceEventTier::kCustomer ? RED : BLACK;
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

// Dashed horizontal rule matching the design doc's hairline gridlines
// (repeating-linear-gradient 2px-on/3px-off) — a solid full-width line read
// as much heavier than the reference and made the grid look denser than it
// is.
void DrawDashedHLine(uint8_t* fb, int width, int y, int x1, int x2, Color color) {
    constexpr int kDash = 2;
    constexpr int kGap = 3;
    for (int x = x1; x < x2; x += kDash + kGap) {
        DrawHLine(fb, width, y, x, std::min(x2, x + kDash), color);
    }
}

// Renders `text` at `scale`x its native bitmap size by rasterizing into a
// small offscreen buffer and blitting each ink pixel as a scale*scale block.
// There's no larger Latin display font in this build (font_zectrix_48_1 is a
// 64-glyph icon font, not text) — this is how the "MEETING"/"FREE" headline
// gets design-doc-sized without adding a new font asset.
void DrawScaledText(uint8_t* fb, int width, int x, int y, const char* text,
                    const lv_font_t* font, Color color, int scale) {
    if (!font || !text || !text[0]) return;
    if (scale <= 1) {
        DrawText(fb, width, x, y, text, font, color);
        return;
    }
    // Deliberately NOT font->line_height/base_line: in this translation unit
    // those fields read back as 0 (a shim-vs-real-LVGL lv_font_t layout
    // mismatch — see font_engine.h). The rest of the codebase never notices
    // because InkCenteredTextTopY + DrawText both fold in the same missing
    // constant and it cancels out; MeasureTextInkBounds is a pure ink
    // top/bottom *difference*, so it's immune to that missing constant and
    // gives the real glyph height here.
    const TextInkBounds bounds = MeasureTextInkBounds(font, text);
    const int tw = std::max(1, MeasureTextWidth(text, font));
    const int th = bounds.valid ? bounds.height : std::max(1, MeasureTextHeight(font));
    const int cursor_y = bounds.valid ? -bounds.top : 0;
    const size_t bytes_per_row = (static_cast<size_t>(tw) * 2 + 7) / 8;
    std::vector<uint8_t> temp(bytes_per_row * static_cast<size_t>(th), 0x55);  // all WHITE (code 1)
    DrawText(temp.data(), tw, 0, cursor_y, text, font, BLACK);
    for (int ty = 0; ty < th; ++ty) {
        for (int tx = 0; tx < tw; ++tx) {
            if (get_pixel_2bpp(temp.data(), tw, tx, ty) != BLACK) continue;
            DrawRect(fb, width, {x + tx * scale, y + ty * scale, scale, scale}, color);
        }
    }
}

int ScaledTextHeight(const lv_font_t* font, int scale) {
    const TextInkBounds bounds = MeasureTextInkBounds(font, "Mg");
    const int th = bounds.valid ? bounds.height : MeasureTextHeight(font);
    return th * std::max(1, scale);
}

// Small camera pictogram (body + viewfinder bump + lens) with an "ON"/"OFF"
// label — always rendered (not just when active) so the door reader never
// has to infer camera state from its absence, matching the design doc's
// always-visible cam badge.
int CameraGlyphWidth(bool on, const lv_font_t* font) {
    constexpr int kIconW = 16;
    constexpr int kGap = 5;
    const char* label = on ? i18n::Tr(i18n::StringId::kBusyLightCamOn) : i18n::Tr(i18n::StringId::kBusyLightCamOff);
    return kIconW + kGap + MeasureTextWidth(label, font);
}

// Faux-bold via a 1px horizontal double-strike — there's no separate Bold
// weight in this build (only Regular/Medium), and this is enough to make the
// tier-carrying word (e.g. "Customer", "important") stand out from the plain
// text next to it.
void DrawTextBold(uint8_t* fb, int width, int x, int y, const char* text,
                  const lv_font_t* font, Color color) {
    DrawText(fb, width, x, y, text, font, color);
    DrawText(fb, width, x + 1, y, text, font, color);
}

void DrawCameraGlyph(uint8_t* fb, int width, int height, int x, int center_y, bool on,
                     const lv_font_t* font, Color base_color) {
    // CAM ON is always red, regardless of the caller's default text color —
    // a live camera should read as an alert everywhere it appears.
    const Color color = on ? RED : base_color;
    constexpr int kIconW = 20;
    constexpr int kIconH = 14;
    constexpr int kGap = 6;
    const int icon_y = center_y - kIconH / 2;
    Rect body{x, icon_y, kIconW, kIconH};
    DrawRoundRect(fb, width, height, body, 2, color, color, 1);
    Rect bump{x + kIconW - 8, icon_y - 4, 6, 5};
    DrawRect(fb, width, bump, color);
    // Lens rendered as a white detail line (filled white, thin color ring)
    // on top of the solid body, instead of the whole icon being a hollow
    // outline — a thin outline read as too faint next to the bold label.
    const Point lens_c{x + kIconW / 2 - 1, icon_y + kIconH / 2};
    DrawCircle(fb, width, lens_c, 4, WHITE);
    DrawCircleBorder(fb, width, lens_c, 4, 1, color);
    if (!on) {
        DrawLine(fb, width, {x - 1, icon_y - 4}, {x + kIconW + 1, icon_y + kIconH + 3}, WHITE);
        DrawLine(fb, width, {x - 1, icon_y - 3}, {x + kIconW + 1, icon_y + kIconH + 4}, WHITE);
    }
    const char* label = on ? i18n::Tr(i18n::StringId::kBusyLightCamOn) : i18n::Tr(i18n::StringId::kBusyLightCamOff);
    DrawTextBold(fb, width, x + kIconW + kGap, InkCenteredTextTopY(font, label, center_y, 0), label, font, color);
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
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);

    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, bg_style);

    if (!current_.valid) {
        const char* empty_text = i18n::Tr(i18n::StringId::kBusyLightNoCalendarData);
        const char* hint = i18n::Tr(i18n::StringId::kBusyLightLongPressRefresh);
        int center_y = Style::kStatusBarHeight + (height - Style::kStatusBarHeight) / 2;
        int text_w = MeasureTextWidth(empty_text, font_);
        int hint_w = MeasureTextWidth(hint, font_);
        DrawText(fb, width, (width - text_w) / 2,
                 InkCenteredTextTopY(font_, empty_text, center_y - 10, 0), empty_text, font_, text);
        DrawText(fb, width, (width - hint_w) / 2,
                 InkCenteredTextTopY(font_, hint, center_y + 16, 0), hint, font_, secondary);
        needs_full_refresh_ = false;
        return;
    }

    if (view_ == View::kDetail) {
        RenderDetailFace(fb, width, height);
    } else {
        RenderDefaultFace(fb, width, height);
    }

    needs_full_refresh_ = false;
}

void BusyLightRenderer::RenderDetailFace(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    const int grid_bottom = height - kGridBottomMargin;
    const int now_minutes = CurrentLocalMinutes();

    // Zoomed viewport, not the whole day: the design doc shows a handful of
    // hours at a time and scrolls that window through the day as "now"
    // advances, rather than compressing 7am-7pm onto one screen (which made
    // a 30-minute meeting a couple of pixels tall). "Now" is centered in the
    // window — deliberately not hour-snapped, since snapping the start to an
    // hour boundary shoved "now" off-center by up to half an hour. The grid
    // loop below handles a non-hour-aligned window_start/window_end directly
    // (partial first/last rows) instead of assuming uniform 60-minute rows.
    constexpr int kZoomSpanMinutes = 6 * 60;
    int window_start = kWindowStartMin;
    int window_end = window_start + kZoomSpanMinutes;
    if (now_minutes >= 0) {
        window_start = std::max(kWindowStartMin, std::min(kWindowEndMin - kZoomSpanMinutes,
                                                            now_minutes - kZoomSpanMinutes / 2));
        window_end = window_start + kZoomSpanMinutes;
    }

    const bool eff_cam = kDebugAvStates[debug_av_index_].cam;
    const bool eff_presenting = kDebugAvStates[debug_av_index_].presenting;
    const EffectiveStatus status = ComputeEffectiveStatus(debug_tier_, eff_cam, eff_presenting);
    const bool busy_now = status.busy;

    // Single-line header: a tier swatch, the headline word, and the camera
    // pictogram — the old two-row status chip ate too much of the panel's
    // height when the grid is the point of this face.
    const Color swatch_color = busy_now ? TierColor(status.tier) : WHITE;
    Rect swatch{Style::kSpacingMD, kHeaderTop + 6, 10, kHeaderHeight - 12};
    DrawRect(fb, width, swatch, swatch_color);
    DrawRectBorder(fb, width, swatch, 1, BLACK);

    const char* word = busy_now ? i18n::Tr(i18n::StringId::kBusyLightMeeting) : i18n::Tr(i18n::StringId::kBusyLightFree);
    const int word_x = swatch.x + swatch.w + Style::kSpacingSM;
    DrawText(fb, width, word_x, InkCenteredTextTopYInBox(title_font_, word, kHeaderTop, kHeaderHeight, 0),
             word, title_font_, BLACK);

    const int cam_w = CameraGlyphWidth(eff_cam, font_);
    DrawCameraGlyph(fb, width, height, width - Style::kSpacingLG - cam_w, kHeaderTop + kHeaderHeight / 2,
                    eff_cam, font_, BLACK);

    // Heavy rule under the header, matching the design's app-bar/grid split.
    DrawHLine(fb, width, kGridTop - 1, 0, width - 1, border);
    DrawHLine(fb, width, kGridTop - 2, 0, width - 1, border);

    // Hour grid: dashed hairlines (matches the design doc's gridline style —
    // a solid full-width rule reads much heavier than the reference). Event
    // blocks below are opaque fills, so they still fully cover the dashes
    // underneath. Iterates absolute hour boundaries rather than assuming
    // window_start is hour-aligned (it isn't, now that the window centers on
    // "now" exactly) — the first/last rows are however much of that hour is
    // actually inside the window, and each label centers within its own
    // (possibly partial) visible row instead of straddling the boundary line.
    const int first_hour = window_start / 60;
    const int last_hour = (window_end - 1) / 60;
    for (int h = first_hour; h <= last_hour; ++h) {
        const int row_start_min = std::max(window_start, h * 60);
        const int row_end_min = std::min(window_end, (h + 1) * 60);
        const int row_top = MinutesToY(row_start_min, window_start, window_end, kGridTop, grid_bottom);
        const int row_bottom = MinutesToY(row_end_min, window_start, window_end, kGridTop, grid_bottom);
        if (row_start_min > window_start) {
            DrawDashedHLine(fb, width, row_top, kLeftMargin, width - kRightMargin, border);
        }
        char label[8];
        snprintf(label, sizeof(label), "%d", h);
        DrawText(fb, width, 4, InkCenteredTextTopYInBox(font_, label, row_top, std::min(16, row_bottom - row_top), 0),
                 label, font_, secondary);
    }

    // Events (2-column overlap layout). Column width is decided per event —
    // only events that actually overlap something get squeezed to half width;
    // an unrelated event elsewhere in the day shouldn't force every row to
    // split. Fill carries importance: outline = internal, yellow + heavy rule
    // = leadership, solid red = customer; anything already finished renders
    // as a black/white hatch regardless of tier, so a glance at the grid
    // tells you what's over without reading a single title.
    //
    // Events entirely outside the zoomed window aren't laid out at all —
    // they're summarized as "+N earlier"/"+N later" instead (see below).
    std::vector<PresenceEvent> visible_events;
    int earlier_count = 0, later_count = 0;
    for (const auto& ev : current_.events) {
        if (ev.end_minutes <= window_start) { ++earlier_count; continue; }
        if (ev.start_minutes >= window_end) { ++later_count; continue; }
        visible_events.push_back(ev);
    }
    std::vector<EventLayout> laid_out;
    int overflow_count = 0;
    LayoutEvents(visible_events, &laid_out, &overflow_count);
    const int area_x0 = kLeftMargin + 3;
    const int area_w = width - kRightMargin - area_x0;
    for (const auto& layout : laid_out) {
        const auto& ev = *layout.event;
        const int y0 = MinutesToY(ev.start_minutes, window_start, window_end, kGridTop, grid_bottom);
        const int y1 = std::max(y0 + 3, MinutesToY(ev.end_minutes, window_start, window_end, kGridTop, grid_bottom));
        const bool split = HasOppositeColumnOverlap(laid_out, layout);
        const int col_w = split ? area_w / 2 - 2 : area_w;
        const int x0 = area_x0 + (split && layout.column == 1 ? area_w / 2 + 2 : 0);
        Rect r{x0, y0, col_w, y1 - y0};

        const bool is_past = ev.end_minutes <= now_minutes;
        Color title_color = BLACK;
        if (is_past) {
            DrawStripeRect(fb, width, r);
            DrawRectBorder(fb, width, r, 1, BLACK);
        } else {
            switch (ev.tier) {
                case PresenceEventTier::kCustomer:
                    DrawRect(fb, width, r, RED);
                    DrawRectBorder(fb, width, r, 1, BLACK);
                    title_color = WHITE;
                    break;
                case PresenceEventTier::kLeadership:
                    DrawRect(fb, width, r, YELLOW);
                    DrawRectBorder(fb, width, r, 1, BLACK);
                    // Heavy top/bottom rule reads as "important" even if the
                    // yellow fill itself doesn't survive a bad print/photo.
                    DrawRect(fb, width, {r.x, r.y, r.w, std::min(3, r.h)}, BLACK);
                    DrawRect(fb, width, {r.x, std::max(r.y, r.y + r.h - 3), r.w, std::min(3, r.h)}, BLACK);
                    break;
                case PresenceEventTier::kInternal:
                default:
                    DrawRect(fb, width, r, WHITE);
                    DrawRectBorder(fb, width, r, 1, BLACK);
                    break;
            }
        }

        // Height gate only (no minimum-duration rule): the design shows
        // titles+times on 30-minute meetings just fine as long as the box is
        // tall enough to hold a line of text.
        if (r.h >= MeasureTextHeight(font_) + 4) {
            char time_buf[16];
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
                     static_cast<int>(ev.start_minutes / 60), static_cast<int>(ev.start_minutes % 60));
            const int time_w = is_past ? 0 : MeasureTextWidth(time_buf, font_);
            const int title_max_w = col_w - 6 - (time_w > 0 ? time_w + Style::kSpacingSM : 0);
            std::string title = TruncateToWidth(ev.title.empty() ? i18n::Tr(i18n::StringId::kBusyLightBusyTitle) : ev.title,
                                                font_, title_max_w);
            // Must go through the ink-centering helper, not a raw y offset:
            // this font's glyph box_h/ofs_y don't map to intuitive top-left
            // coordinates (DrawText's internal glyph placement is baseline-
            // anchored), so `r.y + 2` landed the actual ink ~20px away from
            // the box. Center the ink within a virtual title-row a few px
            // below the box top so ascenders (D, l, h, t) get clearance from
            // the top edge instead of touching/appearing flattened against it.
            constexpr int kTitleRowHeight = 20;
            const int title_y = InkCenteredTextTopYInBox(font_, title.c_str(), r.y + 2, kTitleRowHeight, 0);
            DrawText(fb, width, r.x + 3, title_y, title.c_str(), font_, title_color);
            if (time_w > 0) {
                DrawText(fb, width, r.x + col_w - 3 - time_w, title_y, time_buf, font_, title_color);
            }
        }
    }
    if (overflow_count > 0) {
        char more_buf[24];
        snprintf(more_buf, sizeof(more_buf), i18n::Tr(i18n::StringId::kBusyLightMoreFmt), overflow_count);
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

    // "+N earlier"/"+N later": this is now a zoomed few-hour viewport, not
    // the whole day, so anything scrolled off the top or bottom needs an
    // explicit hint that it still exists rather than silently vanishing.
    if (earlier_count > 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), i18n::Tr(i18n::StringId::kBusyLightEarlierFmt), earlier_count);
        const int w = MeasureTextWidth(buf, font_);
        const int x = width - kRightMargin - 4 - w;
        const int box_top = kGridTop + 2;
        DrawRect(fb, width, {x - 3, box_top, w + 6, 16}, WHITE);
        DrawText(fb, width, x, InkCenteredTextTopYInBox(font_, buf, box_top, 16, 0), buf, font_, secondary);
    }
    if (later_count > 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), i18n::Tr(i18n::StringId::kBusyLightLaterFmt), later_count);
        const int w = MeasureTextWidth(buf, font_);
        const int box_top = grid_bottom - 18;
        DrawRect(fb, width, {kLeftMargin, box_top, w + 6, 16}, WHITE);
        DrawText(fb, width, kLeftMargin + 3, InkCenteredTextTopYInBox(font_, buf, box_top, 16, 0), buf, font_, secondary);
    }

    // Now marker: a black flag in the gutter — the zoomed window is anchored
    // around "now" (see window_start above), so this should almost always be
    // visible somewhere in the visible range.
    if (now_minutes >= window_start && now_minutes <= window_end) {
        const int y = MinutesToY(now_minutes, window_start, window_end, kGridTop, grid_bottom);
        constexpr int kFlagW = 10;
        DrawHLine(fb, width, y, kLeftMargin - kFlagW - 2, kLeftMargin - 2, BLACK);
        DrawHLine(fb, width, y + 1, kLeftMargin - kFlagW - 2, kLeftMargin - 2, BLACK);
        for (int i = 0; i < 5; ++i) {
            DrawHLine(fb, width, y - 4 + i, kLeftMargin - 5 - i, kLeftMargin - 2, BLACK);
            DrawHLine(fb, width, y + 4 - i, kLeftMargin - 5 - i, kLeftMargin - 2, BLACK);
        }
    }
}

void BusyLightRenderer::RenderDefaultFace(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);

    const int now_minutes = CurrentLocalMinutes();
    // "until" and the rail always reflect the real calendar; only the
    // word/band/human-line/camera/presenting below are driven by the
    // debug override (UP/DOWN), so the debug cycle never has to fake a
    // fictitious end time or corrupt the actual day shape.
    const PresenceEvent* real_active = ActiveEvent(current_, now_minutes);
    const bool eff_cam = kDebugAvStates[debug_av_index_].cam;
    const bool eff_presenting = kDebugAvStates[debug_av_index_].presenting;
    const EffectiveStatus status = ComputeEffectiveStatus(debug_tier_, eff_cam, eff_presenting);
    const bool busy_now = status.busy;

    // Top status band: red whenever the room is elevated (leadership,
    // customer, an ad-hoc call, or a screen-share) — plain internal meetings
    // and free time leave it white/invisible. Full width and tall, matching
    // the design doc; always reserves the same height so the rest of the
    // layout doesn't shift depending on state. A thin border under it even
    // when white, so the free state still reads as a deliberate empty band
    // rather than a missing element.
    constexpr int kBandH = 26;
    DrawRect(fb, width, {0, Style::kStatusBarHeight, width, kBandH}, status.band_elevated ? RED : WHITE);
    DrawHLine(fb, width, Style::kStatusBarHeight + kBandH, 0, width - 1, border);

    const int content_top = Style::kStatusBarHeight + kBandH;
    const int footer_h = 30;
    const int footer_top = height - footer_h;

    // Left rail: the shape of the whole day (kHourStart..kHourEnd) without a
    // single label — "and later?" answered for free by whoever glances at it
    // from the hallway. Runs the full remaining height of the panel (not
    // stopping above the footer): in the design doc the rail and the footer
    // row are side-by-side flex children of the same height, so the rail
    // continues alongside the footer text rather than ending above it.
    // track_top/track_bottom leave a dedicated gap above and below the
    // bracket for the hour labels — centering a label directly on the
    // bracket's own edge made it overlap the band above / the bracket itself
    // below, instead of just being close to it.
    constexpr int kRailW = 44;
    constexpr int kTrackW = 16;
    // 28, not a smaller round number: the bottom label needs at least ~14px
    // clearance from the true panel edge (the global page frame rounds off
    // that corner and clips anything hugging it), so half the gap must clear
    // that on its own.
    constexpr int kLabelGap = 28;
    const int track_x = (kRailW - kTrackW) / 2;
    const int track_top = content_top + kLabelGap;
    const int track_bottom = height - kLabelGap;
    DrawVLine(fb, width, kRailW, content_top, height - 1, border);
    DrawRectBorder(fb, width, {track_x, track_top, kTrackW, track_bottom - track_top}, 1, border);
    for (const auto& ev : current_.events) {
        if (ev.end_minutes <= kWindowStartMin || ev.start_minutes >= kWindowEndMin) continue;
        const int y0 = MinutesToY(ev.start_minutes, kWindowStartMin, kWindowEndMin, track_top, track_bottom);
        const int y1 = std::max(y0 + 1, MinutesToY(ev.end_minutes, kWindowStartMin, kWindowEndMin, track_top, track_bottom));
        DrawRect(fb, width, {track_x + 1, y0, kTrackW - 2, y1 - y0}, RailColor(ev.tier));
    }
    if (now_minutes >= kWindowStartMin && now_minutes <= kWindowEndMin) {
        const int now_y = MinutesToY(now_minutes, kWindowStartMin, kWindowEndMin, track_top, track_bottom);
        DrawHLine(fb, width, now_y, track_x - 6, track_x + kTrackW + 6, BLACK);
        DrawHLine(fb, width, now_y + 1, track_x - 6, track_x + kTrackW + 6, BLACK);
    }
    // Centered on the rail's full width (not track_x), and vertically
    // centered within the dedicated kLabelGap band above/below the bracket —
    // not just offset from the bracket's edge, which left no real margin
    // once the band/footer moved the bracket's own edges closer to it.
    char hour_label[8];
    snprintf(hour_label, sizeof(hour_label), "%d", kHourStart);
    int label_w = MeasureTextWidth(hour_label, font_);
    const int top_label_y = content_top + kLabelGap / 2;
    DrawText(fb, width, (kRailW - label_w) / 2, InkCenteredTextTopY(font_, hour_label, top_label_y, 0),
             hour_label, font_, secondary);
    snprintf(hour_label, sizeof(hour_label), "%d", kHourEnd);
    label_w = MeasureTextWidth(hour_label, font_);
    const int bottom_label_y = track_bottom + kLabelGap / 2;
    DrawText(fb, width, (kRailW - label_w) / 2, InkCenteredTextTopY(font_, hour_label, bottom_label_y, 0),
             hour_label, font_, secondary);

    // Right content column.
    const int content_x0 = kRailW + Style::kSpacingMD;
    const int content_right = width - Style::kSpacingMD;

    // Headline word, drawn 3x native size — this build has no larger Latin
    // display font (font_zectrix_48_1 is a 64-glyph icon font, not text), so
    // DrawScaledText blits the 24px title font up to design-doc scale. A
    // bigger gap follows it, matching the design doc's spacing between the
    // headline and the color band above it.
    int y = content_top + Style::kSpacingLG;
    constexpr int kWordScale = 3;
    const char* word = busy_now ? i18n::Tr(i18n::StringId::kBusyLightMeeting) : i18n::Tr(i18n::StringId::kBusyLightFree);
    DrawScaledText(fb, width, content_x0, y, word, title_font_, text, kWordScale);
    y += ScaledTextHeight(title_font_, kWordScale) + Style::kSpacingLG;

    // "until HH:MM" — always a wall-clock time, never a countdown, so a
    // 5-minute refresh cadence never shows something stale. Always the real
    // calendar, even while UP/DOWN is previewing a different debug status.
    // title_font_ (Medium, 24px), not font_: bigger and bolder, and there's
    // headroom now that the word/band/gaps grew to match the design doc.
    constexpr int kLineBoxH = 26;
    std::string until_line;
    if (real_active) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s %02d:%02d", i18n::Tr(i18n::StringId::kBusyLightUntil),
                 static_cast<int>(real_active->end_minutes / 60), static_cast<int>(real_active->end_minutes % 60));
        until_line = buf;
    } else {
        const PresenceEvent* next = NextEvent(current_, now_minutes);
        if (next) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s %02d:%02d", i18n::Tr(i18n::StringId::kBusyLightUntilNext),
                     static_cast<int>(next->start_minutes / 60), static_cast<int>(next->start_minutes % 60));
            until_line = buf;
        } else {
            until_line = i18n::Tr(i18n::StringId::kBusyLightFreeAllDay);
        }
    }
    const int until_center_y = y + kLineBoxH / 2;
    DrawText(fb, width, content_x0, InkCenteredTextTopYInBox(title_font_, until_line.c_str(), y, kLineBoxH, 0),
             until_line.c_str(), title_font_, secondary);
    // Camera badge lines up with "until", not the human line below: matching
    // font sizes (both small/secondary-weight) put it on a coherent visual
    // line, where next to the big bold human-line text it looked mismatched.
    const int cam_w = CameraGlyphWidth(eff_cam, font_);
    DrawCameraGlyph(fb, width, height, content_right - Style::kSpacingSM - cam_w, until_center_y,
                    eff_cam, font_, secondary);
    y += kLineBoxH + Style::kSpacingSM;

    // Human line — the tier-carrying segment (e.g. "important") is colored
    // AND bold, so it stands out from the plain segment next to it, not just
    // by color.
    std::string seg_a, seg_b;
    Color seg_b_color = text;
    if (status.is_adhoc) {
        // Nothing on the calendar, but camera/presenting is live — the design
        // doc's own "ad-hoc call" rule: never silently reads as FREE.
        seg_b = i18n::Tr(i18n::StringId::kBusyLightAdhocCall);
        seg_b_color = RED;
    } else if (!busy_now) {
        seg_b = i18n::Tr(i18n::StringId::kBusyLightComeOnIn);
    } else {
        switch (status.tier) {
            case PresenceEventTier::kLeadership:
                seg_a = i18n::Tr(i18n::StringId::kBusyLightInternalPrefix);
                seg_b = i18n::Tr(i18n::StringId::kBusyLightImportant);
                seg_b_color = RED;
                break;
            case PresenceEventTier::kCustomer:
                seg_b = i18n::Tr(i18n::StringId::kBusyLightCustomer);
                seg_b_color = RED;
                break;
            case PresenceEventTier::kInternal:
            default:
                seg_a = i18n::Tr(i18n::StringId::kBusyLightInternalPrefix);
                seg_b = i18n::Tr(i18n::StringId::kBusyLightComeIn);
                break;
        }
    }
    const int human_center_y = y + kLineBoxH / 2;
    // One shared y for both segments, not each ink-centered independently:
    // "important" (with its descender) and "Internal — " measure different
    // ink bounds, so centering them separately put them a couple of pixels
    // off each other's baseline instead of reading as one line.
    const int human_y = InkCenteredTextTopY(title_font_, "Ag", human_center_y, 0);
    int seg_x = content_x0;
    if (!seg_a.empty()) {
        DrawText(fb, width, seg_x, human_y, seg_a.c_str(), title_font_, text);
        seg_x += MeasureTextWidth(seg_a.c_str(), title_font_);
    }
    DrawTextBold(fb, width, seg_x, human_y, seg_b.c_str(), title_font_, seg_b_color);
    y += kLineBoxH + Style::kSpacingMD;

    // "Presenting" banner: demo-only signal (no real screen-share sensor
    // wired up yet) shown whenever the effective presenting state is set.
    if (eff_presenting) {
        constexpr int kBannerH = 22;
        Rect banner{content_x0, y, content_right - content_x0, kBannerH};
        DrawRect(fb, width, banner, YELLOW);
        DrawRectBorder(fb, width, banner, 1, BLACK);
        const char* banner_text = i18n::Tr(i18n::StringId::kBusyLightPresentingBanner);
        const int banner_text_w = MeasureTextWidth(banner_text, font_);
        DrawText(fb, width, banner.x + std::max(4, (banner.w - banner_text_w) / 2),
                 InkCenteredTextTopYInBox(font_, banner_text, banner.y, banner.h, 0), banner_text, font_, BLACK);
    }

    // Footer: hints that BOOT opens the detail face. Divider only spans the
    // content column, not the rail — in the design doc the rail and the
    // content column (footer included) are side-by-side flex children, so
    // the footer's own top border never crosses into the rail.
    DrawHLine(fb, width, footer_top, kRailW, width - 1, border);
    const char* footer_hint = i18n::Tr(i18n::StringId::kBusyLightFooterHint);
    const int hint_w = MeasureTextWidth(footer_hint, font_);
    constexpr int kChevronDiameter = 17;
    const int chevron_x = width - Style::kSpacingMD - kChevronDiameter;
    const int hint_x = chevron_x - Style::kSpacingSM - hint_w;
    DrawText(fb, width, hint_x, InkCenteredTextTopYInBox(font_, footer_hint, footer_top, footer_h, 0),
             footer_hint, font_, secondary);
    const Point chevron_c{chevron_x + kChevronDiameter / 2, footer_top + footer_h / 2};
    DrawCircle(fb, width, chevron_c, kChevronDiameter / 2, YELLOW);
    DrawCircleBorder(fb, width, chevron_c, kChevronDiameter / 2, 1, BLACK);
    DrawText(fb, width, chevron_c.x - 3, InkCenteredTextTopY(font_, ">", chevron_c.y, 0), ">", font_, BLACK);
}

bool BusyLightRenderer::HandleInput(const ButtonEvent& event) {
    switch (event.type) {
        case ButtonEvent::kBootClick:
            view_ = (view_ == View::kDefault) ? View::kDetail : View::kDefault;
            needs_full_refresh_ = true;
            return true;
        // Debug/demo cycles — neither button did anything on this page
        // before. UP steps through the four status tiers, DOWN through the
        // camera/presenting combinations; both only affect the header (word,
        // swatch, band, human line, camera, presenting banner), never the
        // real calendar underneath.
        case ButtonEvent::kUpClick:
            debug_tier_ = static_cast<DebugTier>((static_cast<int>(debug_tier_) + 1) % 4);
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kDownClick:
            debug_av_index_ = (debug_av_index_ + 1) % kDebugAvStateCount;
            needs_full_refresh_ = true;
            return true;
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
