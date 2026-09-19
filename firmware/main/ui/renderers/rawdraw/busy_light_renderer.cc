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

#include "board.h"
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

// Detail face's zoomed viewport span and UP/DOWN scroll step — see
// RenderDetailFace and BusyLightRenderer::ScrollDetailWindow.
constexpr int kZoomSpanMinutes = 6 * 60;
constexpr int kDetailScrollStepMinutes = 60;

// Shared with the change-detection signature below (RenderDefaultFace used
// to have its own local copy of this literal).
constexpr int kEndOfWorkdayMinutes = 17 * 60;

int ClampWindowStart(int start) {
    return std::max(kWindowStartMin, std::min(kWindowEndMin - kZoomSpanMinutes, start));
}

int AutoWindowStart(int now_minutes) {
    if (now_minutes < 0) return kWindowStartMin;
    return ClampWindowStart(now_minutes - kZoomSpanMinutes / 2);
}

struct EventLayout {
    const PresenceEvent* event;
    int column;
};

// The page now always renders against the real RTC/SNTP-synced wall-clock
// time (calendar data itself has come from a real fetch since
// presence_api_init in application.cc). This used to default to a fixed
// 10:30am for dev/screenshot sessions where real time rarely fell inside
// business hours — flip back to true temporarily if that's needed again,
// but production builds must ship with this false.
constexpr bool kMockCurrentTime = false;
constexpr int kMockCurrentTimeMinutes = 10 * 60 + 30;  // 10:30am, only used if kMockCurrentTime is true

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

#if CONFIG_BUSY_LIGHT_DEBUG_CYCLE
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
#endif  // CONFIG_BUSY_LIGHT_DEBUG_CYCLE

// Whether this tier is important enough to color the top status band red.
bool IsElevated(PresenceEventTier tier) {
    return tier != PresenceEventTier::kInternal && tier != PresenceEventTier::kSolo;
}

// Effective header status, folding the real calendar's active event together
// with live call/webcam/presenting state. Shared by both faces' headers so
// "ad-hoc call" (nothing on the calendar, but a call/camera/screen-share is
// live — see the design doc's own `adhoc` rule) is computed identically
// everywhere.
struct EffectiveStatus {
    bool busy;
    PresenceEventTier tier;   // meaningful only if busy
    bool band_elevated;       // top band / swatch should read as elevated (red)
    bool is_adhoc;
};

EffectiveStatus ComputeEffectiveStatus(bool calendar_busy, PresenceEventTier calendar_tier,
                                        bool in_call, bool webcam_active, bool presenting) {
    EffectiveStatus s;
    // Ad-hoc call: nothing on the calendar, but a call/camera/screen-share is
    // live — the design doc treats that as a customer-severity call
    // ("Ad-hoc call", never silently reads as FREE) rather than pretending
    // nobody's in the room. in_call counts here even though it has no
    // dedicated glyph of its own (that's webcam_active's job below).
    s.is_adhoc = !calendar_busy && (in_call || webcam_active || presenting);
    s.busy = calendar_busy || s.is_adhoc;
    s.tier = s.is_adhoc ? PresenceEventTier::kCustomer : calendar_tier;
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

// Field-by-field comparison (title included: the detail-face grid renders
// it — see the title truncation in RenderDetailFace's grid loop). Used by
// BusyLightRenderer::Update() to decide whether a poll actually changed
// anything worth redrawing for.
bool EventsEqual(const std::vector<PresenceEvent>& a, const std::vector<PresenceEvent>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].start_minutes != b[i].start_minutes ||
            a[i].end_minutes != b[i].end_minutes ||
            a[i].tier != b[i].tier ||
            a[i].title != b[i].title) {
            return false;
        }
    }
    return true;
}

// Everything time-derived that can change what's on screen even when the
// raw PresenceStatus payload is byte-identical to the last redraw — e.g.
// "now" crossing a meeting's start/end minute (flips the active/next event,
// and each grid row's past/future styling), or the 5pm tomorrow-line cutoff.
// past_event_count (rather than tracking every event's past/future flag
// individually) is enough to catch any such crossing: within one fixed
// events list, it can only increase as now_minutes advances.
//
// NOTE: this deliberately does NOT make the "now" line/tick itself move on
// every minute — it only moves when Update() is called (poll-driven, same
// as before this change), and now only redraws when something above
// actually crossed a boundary. That's an intentional trade-off once
// kMockCurrentTime flips to real wall-clock time: the now-marker reads a
// few pixels stale between meaningful state changes rather than forcing a
// full e-ink refresh every poll just to nudge it. See
// docs/optimizations-todo.md.
struct TimeDerivedSignature {
    int32_t active_start = -1;
    int32_t active_end = -1;
    int32_t next_start = -1;
    int32_t past_event_count = 0;
    bool tomorrow_banner_visible = false;
};

TimeDerivedSignature ComputeTimeDerivedSignature(const PresenceStatus& status, int now_minutes) {
    TimeDerivedSignature sig;
    const PresenceEvent* active = ActiveEvent(status, now_minutes);
    const PresenceEvent* next = NextEvent(status, now_minutes);
    sig.active_start = active ? active->start_minutes : -1;
    sig.active_end = active ? active->end_minutes : -1;
    sig.next_start = next ? next->start_minutes : -1;
    for (const auto& ev : status.events) {
        if (ev.end_minutes <= now_minutes) sig.past_event_count++;
    }
    sig.tomorrow_banner_visible = status.tomorrow_valid && now_minutes >= kEndOfWorkdayMinutes;
    return sig;
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

// Yellow-fill "highlight chip" behind bold black text, matching the
// leadership-tier event fill and the presenting banner: yellow *text*
// directly on the page background reads with too little contrast on e-ink
// to be legible, so importance here is carried by the fill instead, same
// as everywhere else in this renderer. Returns the chip's width so callers
// can position further text after it. NOT MeasureTextHeight(font): that
// reads font->line_height, which is 0 in this translation unit (same shim/
// LVGL layout mismatch called out in DrawScaledText's comment above), and
// a chip_h of ~4px renders as a thin strike-through line, not a box.
// text_y is the caller's already-computed shared baseline (see the
// "one shared y for both segments" comment at the call site) — NOT
// recomputed here from `text`'s own ink bounds. Different strings have
// slightly different ink bounds, so centering each chip on its own text
// independently put boxed and plain segments on the same line a pixel or
// two off each other's baseline, the same class of bug the shared-y
// comment already exists to prevent.
int DrawHighlightChip(uint8_t* fb, int width, int x, int center_y, int text_y, const char* text, const lv_font_t* font) {
    if (!font || !text || !text[0]) return 0;
    constexpr int kChipPadX = 4;
    constexpr int kChipPadY = 3;
    const TextInkBounds bounds = MeasureTextInkBounds(font, "Ag");
    const int text_h = bounds.valid ? bounds.height : 12;
    // +1 for DrawTextBold's double-strike offset, so the bold stroke's
    // rightmost pixel doesn't land right on (or past) the chip's edge.
    const int text_w = MeasureTextWidth(text, font) + 1;
    const int chip_h = text_h + kChipPadY * 2;
    Rect chip{x, center_y - chip_h / 2, text_w + kChipPadX * 2, chip_h};
    DrawRect(fb, width, chip, YELLOW);
    DrawTextBold(fb, width, x + kChipPadX, text_y, text, font, BLACK);
    return chip.w;
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
    // window by default — deliberately not hour-snapped, since snapping the
    // start to an hour boundary shoved "now" off-center by up to half an
    // hour. UP/DOWN (see ScrollDetailWindow) can override that auto-centering
    // with an explicit window_start. The grid loop below handles a
    // non-hour-aligned window_start/window_end directly (partial first/last
    // rows) instead of assuming uniform 60-minute rows.
    const int window_start = (detail_window_start_override_ >= 0)
        ? detail_window_start_override_ : AutoWindowStart(now_minutes);
    const int window_end = window_start + kZoomSpanMinutes;

    const PresenceEvent* real_active = ActiveEvent(current_, now_minutes);
    bool calendar_busy = real_active != nullptr;
    PresenceEventTier calendar_tier = real_active ? real_active->tier : PresenceEventTier::kInternal;
    bool webcam_active = current_.webcam_active;
    bool presenting = current_.presenting;
#if CONFIG_BUSY_LIGHT_DEBUG_CYCLE
    calendar_busy = DebugTierIsBusy(debug_tier_);
    calendar_tier = DebugTierToPresenceTier(debug_tier_);
    webcam_active = kDebugAvStates[debug_av_index_].cam;
    presenting = kDebugAvStates[debug_av_index_].presenting;
#endif  // CONFIG_BUSY_LIGHT_DEBUG_CYCLE
    const EffectiveStatus status = ComputeEffectiveStatus(
        calendar_busy, calendar_tier, current_.in_call, webcam_active, presenting);
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

    const int cam_w = CameraGlyphWidth(webcam_active, font_);
    DrawCameraGlyph(fb, width, height, width - Style::kSpacingLG - cam_w, kHeaderTop + kHeaderHeight / 2,
                    webcam_active, font_, BLACK);

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
        char label[16];
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
    const PresenceEvent* real_active = ActiveEvent(current_, now_minutes);
    bool calendar_busy = real_active != nullptr;
    PresenceEventTier calendar_tier = real_active ? real_active->tier : PresenceEventTier::kInternal;
    bool webcam_active = current_.webcam_active;
    bool presenting = current_.presenting;
#if CONFIG_BUSY_LIGHT_DEBUG_CYCLE
    calendar_busy = DebugTierIsBusy(debug_tier_);
    calendar_tier = DebugTierToPresenceTier(debug_tier_);
    webcam_active = kDebugAvStates[debug_av_index_].cam;
    presenting = kDebugAvStates[debug_av_index_].presenting;
    // Fabricate an "until"/"until next" event to match the debug word/tier
    // above — without this, the real calendar (often empty at whatever the
    // real current time happens to be) leaves the "MEETING" headline sitting
    // above a "free all day" second line, which reads as a bug even though
    // it's "working as designed". A fixed 30-minute span straddling "now"
    // mirrors how a real event would be drawn: an end time while busy, a
    // start time (30min out) while free.
    PresenceEvent debug_event{};
    debug_event.tier = calendar_tier;
    constexpr int kDebugEventSpanMinutes = 30;
    if (calendar_busy) {
        debug_event.start_minutes = now_minutes - kDebugEventSpanMinutes;
        debug_event.end_minutes = now_minutes + kDebugEventSpanMinutes;
        real_active = &debug_event;
    } else {
        debug_event.start_minutes = now_minutes + kDebugEventSpanMinutes;
        debug_event.end_minutes = debug_event.start_minutes + kDebugEventSpanMinutes;
        real_active = nullptr;  // debug says free — never fall through to a real active event
    }
#endif  // CONFIG_BUSY_LIGHT_DEBUG_CYCLE
    const EffectiveStatus status = ComputeEffectiveStatus(
        calendar_busy, calendar_tier, current_.in_call, webcam_active, presenting);
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
#if CONFIG_BUSY_LIGHT_DEBUG_CYCLE
        next = &debug_event;
#endif  // CONFIG_BUSY_LIGHT_DEBUG_CYCLE
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
    const int cam_w = CameraGlyphWidth(webcam_active, font_);
    DrawCameraGlyph(fb, width, height, content_right - Style::kSpacingSM - cam_w, until_center_y,
                    webcam_active, font_, secondary);
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

    // "Presenting" banner, shown whenever the screen-share/do-not-disturb
    // signal is set (real, from /live — or the debug override, if enabled).
    if (presenting) {
        constexpr int kBannerH = 22;
        Rect banner{content_x0, y, content_right - content_x0, kBannerH};
        DrawRect(fb, width, banner, YELLOW);
        DrawRectBorder(fb, width, banner, 1, BLACK);
        const char* banner_text = i18n::Tr(i18n::StringId::kBusyLightPresentingBanner);
        const int banner_text_w = MeasureTextWidth(banner_text, font_);
        DrawText(fb, width, banner.x + std::max(4, (banner.w - banner_text_w) / 2),
                 InkCenteredTextTopYInBox(font_, banner_text, banner.y, banner.h, 0), banner_text, font_, BLACK);
    }

    // Tomorrow's first meeting — end-of-workday context so a glance on the
    // way out answers "what does tomorrow morning look like" without
    // opening the detail face. Fixed slot directly above the footer,
    // reserved whether or not it's actually drawn, so nothing else in the
    // layout shifts depending on the time of day. Both branches share one
    // prefix/highlight/suffix shape (rather than one being a special case)
    // because which part reads naturally around the highlighted segment
    // differs by language: English puts nothing before "No meetings" and
    // "tomorrow" after; Chinese puts "明天" (tomorrow) first in both cases.
    constexpr int kTomorrowLineH = 18;
    if (now_minutes >= kEndOfWorkdayMinutes && current_.tomorrow_valid) {
        // kSpacingXS clearance from the footer divider: with no gap, a
        // descender (e.g. the "g" in "meeting") lands right on the divider
        // line and reads as touching/clipped.
        const int tomorrow_line_top = footer_top - Style::kSpacingXS - kTomorrowLineH;
        const int tomorrow_center_y = tomorrow_line_top + kTomorrowLineH / 2;
        // One shared y for every segment on this line, plain and boxed
        // alike (mirrors the human-line comment above): ink-centering each
        // piece independently — including inside the chip — would put them
        // a pixel or two off each other's baseline.
        const int tomorrow_y = InkCenteredTextTopY(font_, "Ag", tomorrow_center_y, 0);

        std::string prefix, highlight, suffix;
        if (current_.tomorrow_first_event_minutes >= 0) {
            prefix = i18n::Tr(i18n::StringId::kBusyLightTomorrowFirstMeetingPrefix);
            char time_buf[16];
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
                     static_cast<int>(current_.tomorrow_first_event_minutes / 60),
                     static_cast<int>(current_.tomorrow_first_event_minutes % 60));
            highlight = time_buf;
            suffix = i18n::Tr(i18n::StringId::kBusyLightTomorrowFirstMeetingSuffix);
        } else {
            prefix = i18n::Tr(i18n::StringId::kBusyLightTomorrowNoMeetingsPrefix);
            highlight = i18n::Tr(i18n::StringId::kBusyLightTomorrowNoMeetingsHighlight);
            suffix = i18n::Tr(i18n::StringId::kBusyLightTomorrowNoMeetingsSuffix);
        }

        // Plain (not bold) prefix/suffix: only the highlighted segment
        // (the time, or "No meetings") should draw the eye, matching the
        // chip's already-bold text — bolding everything flattened that
        // contrast back out.
        int seg_x = content_x0;
        if (!prefix.empty()) {
            DrawText(fb, width, seg_x, tomorrow_y, prefix.c_str(), font_, secondary);
            seg_x += MeasureTextWidth(prefix.c_str(), font_);
        }
        // Chip, not plain yellow text: yellow text directly on the page
        // background has too little contrast to read on e-ink.
        seg_x += DrawHighlightChip(fb, width, seg_x, tomorrow_center_y, tomorrow_y, highlight.c_str(), font_);
        if (!suffix.empty()) {
            DrawText(fb, width, seg_x, tomorrow_y, suffix.c_str(), font_, secondary);
        }
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

void BusyLightRenderer::ScrollDetailWindow(int delta_minutes) {
    const int current_start = (detail_window_start_override_ >= 0)
        ? detail_window_start_override_ : AutoWindowStart(CurrentLocalMinutes());
    const int new_start = ClampWindowStart(current_start + delta_minutes);
    if (new_start == current_start) {
        // Already at the day's start/end — the clamp absorbed the whole
        // step, so this press was a no-op. Application::OnUpClick/
        // OnDownClick already flashes the activity LED unconditionally
        // before HandleInput runs, so signal "rejected" with a rapid
        // double-blink on top of that rather than trying to suppress it.
        Board::GetInstance().FlashErrorLed();
        return;
    }
    detail_window_start_override_ = new_start;
    needs_full_refresh_ = true;
}

bool BusyLightRenderer::HandleInput(const ButtonEvent& event) {
    switch (event.type) {
        case ButtonEvent::kBootClick:
            view_ = (view_ == View::kDefault) ? View::kDetail : View::kDefault;
            // Always re-enter a view at its auto-centered-on-now window
            // rather than remembering where a previous visit to the detail
            // face was scrolled to — simplest mental model, and means a
            // scroll never "gets lost" in a state you have to remember to
            // undo.
            detail_window_start_override_ = -1;
            needs_full_refresh_ = true;
            return true;
        // On the detail face, UP/DOWN scroll the zoomed day-grid window
        // (see RenderDetailFace) one hour at a time; a press that's already
        // at the clamped start/end of the day (kWindowStartMin/kWindowEndMin)
        // is a no-op signaled by a rapid double-blink instead of the usual
        // single activity-pulse blink, so the clamp is felt, not just seen
        // on the next 15-25s e-ink refresh.
#if CONFIG_BUSY_LIGHT_DEBUG_CYCLE
        // On the default face (only compiled in when CONFIG_BUSY_LIGHT_DEBUG_CYCLE
        // is set — see Kconfig.projbuild), UP/DOWN instead cycle the debug/demo
        // status override: UP steps through the four status tiers, DOWN
        // through the camera/presenting combinations. Both only affect the
        // header (word, swatch, band, human line, camera, presenting
        // banner), never the real calendar underneath.
        case ButtonEvent::kUpClick:
            if (view_ == View::kDetail) {
                ScrollDetailWindow(-kDetailScrollStepMinutes);
                return true;
            }
            debug_tier_ = static_cast<DebugTier>((static_cast<int>(debug_tier_) + 1) % 4);
            needs_full_refresh_ = true;
            return true;
        case ButtonEvent::kDownClick:
            if (view_ == View::kDetail) {
                ScrollDetailWindow(kDetailScrollStepMinutes);
                return true;
            }
            debug_av_index_ = (debug_av_index_ + 1) % kDebugAvStateCount;
            needs_full_refresh_ = true;
            return true;
#else   // !CONFIG_BUSY_LIGHT_DEBUG_CYCLE
        case ButtonEvent::kUpClick:
            if (view_ != View::kDetail) return false;
            ScrollDetailWindow(-kDetailScrollStepMinutes);
            return true;
        case ButtonEvent::kDownClick:
            if (view_ != View::kDetail) return false;
            ScrollDetailWindow(kDetailScrollStepMinutes);
            return true;
#endif  // CONFIG_BUSY_LIGHT_DEBUG_CYCLE
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
    const int now_minutes = CurrentLocalMinutes();
    const bool first_update = !has_rendered_once_;

    const bool events_changed = !EventsEqual(last_evaluated_.events, status.events);
    const bool av_changed = last_evaluated_.in_call != status.in_call ||
                             last_evaluated_.webcam_active != status.webcam_active ||
                             last_evaluated_.presenting != status.presenting;
    const bool other_raw_changed = last_evaluated_.valid != status.valid ||
                                    last_evaluated_.tomorrow_valid != status.tomorrow_valid ||
                                    last_evaluated_.tomorrow_first_event_minutes != status.tomorrow_first_event_minutes;

    const TimeDerivedSignature time_sig = ComputeTimeDerivedSignature(status, now_minutes);
    const bool time_derived_changed = time_sig.active_start != last_active_start_ ||
                                       time_sig.active_end != last_active_end_ ||
                                       time_sig.next_start != last_next_start_ ||
                                       time_sig.past_event_count != last_past_event_count_ ||
                                       time_sig.tomorrow_banner_visible != last_tomorrow_banner_visible_;

    const bool calendar_related_changed = events_changed || other_raw_changed || time_derived_changed;
    const bool anything_changed = first_update || calendar_related_changed || av_changed;

    current_ = status;

    if (!anything_changed) {
        // Identical to what's already on screen (modulo last_updated_unix,
        // which nothing renders) — skip the redraw entirely rather than
        // burning a 15-25s e-ink refresh cycle for pixels that wouldn't
        // change. See docs/optimizations-todo.md.
        pending_visible_change_ = false;
        return;
    }

    // On the detail face, an AV-only change (webcam/in_call/presenting)
    // with the calendar grid itself untouched only moves the header strip
    // — the day grid below it doesn't read any of those fields (see
    // RenderDetailFace). The default face has no such split: AV state
    // feeds the band/headline/human-line/presenting-banner across most of
    // the page, so any change there still needs a full-screen redraw.
    if (!first_update && view_ == View::kDetail && !calendar_related_changed) {
        pending_dirty_rect_ = Rect{0, kHeaderTop, width_, kHeaderHeight};
    } else {
        pending_dirty_rect_ = Rect{0, 0, 0, 0};
    }
    pending_visible_change_ = true;
    needs_full_refresh_ = true;

    last_evaluated_ = status;
    last_active_start_ = time_sig.active_start;
    last_active_end_ = time_sig.active_end;
    last_next_start_ = time_sig.next_start;
    last_past_event_count_ = time_sig.past_event_count;
    last_tomorrow_banner_visible_ = time_sig.tomorrow_banner_visible;
    has_rendered_once_ = true;
}

}  // namespace rawdraw
