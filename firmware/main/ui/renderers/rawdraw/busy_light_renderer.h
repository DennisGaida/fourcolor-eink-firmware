/**
 * @file busy_light_renderer.h
 * @brief Busy-light default page renderer for rawdraw mode
 *
 * Day-view calendar (hour rows, event blocks, now-line) plus an in-call /
 * webcam status chip, for the office-door busy-light use case.
 */

#ifndef RAWDRAW_BUSY_LIGHT_RENDERER_H
#define RAWDRAW_BUSY_LIGHT_RENDERER_H

#include "common/presence_types.h"
#include "page_renderer.h"
#include "rawdraw/style.h"

namespace rawdraw {

class BusyLightRenderer : public PageRenderer {
public:
    // The hallway face (glanceable word + day rail) and the detail face (full
    // day grid), toggled by a BOOT click — mirrors Gallery's full/split view
    // toggle. Unlike Gallery there is no auto-timeout back to kDefault: this
    // is a battery e-paper panel and a background timer would force a full
    // refresh with nobody looking at it.
    enum class View {
        kDefault,
        kDetail,
    };

#if CONFIG_BUSY_LIGHT_DEBUG_CYCLE
    // Debug/demo status override for the header (word/swatch/band/human
    // line) — cycled by UP click, gated behind CONFIG_BUSY_LIGHT_DEBUG_CYCLE
    // (see Kconfig.projbuild) since it overrides the real calendar/presence
    // data and has no business being on in a real deployment.
    enum class DebugTier {
        kFree,
        kInternal,
        kLeadership,
        kCustomer,
    };
#endif  // CONFIG_BUSY_LIGHT_DEBUG_CYCLE

    BusyLightRenderer();
    ~BusyLightRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Data interface
    void Update(const PresenceStatus& status);

    // Change-detection result from the last Update() call: whether anything
    // actually visible changed (vs. an identical poll result redrawing the
    // panel for nothing), and if so, whether it can be satisfied with a
    // small dirty rect instead of a full-screen redraw. Consuming clears the
    // pending flag; RawDrawUiManager::UpdatePresenceStatus is the only
    // caller.
    bool HasPendingVisibleChange() const { return pending_visible_change_; }
    Rect ConsumeDirtyRect() {
        pending_visible_change_ = false;
        return pending_dirty_rect_;
    }

private:
    void RenderDefaultFace(uint8_t* fb, int width, int height);
    void RenderDetailFace(uint8_t* fb, int width, int height);
    // Shifts the detail face's zoomed day-grid window by delta_minutes
    // (relative to whatever's currently shown, auto-centered or already
    // scrolled), clamped to the day's [kWindowStartMin, kWindowEndMin]
    // bounds. Flashes the activity LED once on a normal step (via the
    // caller in HandleInput's needs_full_refresh_ path — see
    // Application::OnUpClick/OnDownClick, which always flashes it), or the
    // error LED's rapid double-blink if the clamp absorbed the whole step.
    void ScrollDetailWindow(int delta_minutes);

    PresenceStatus current_;
    View view_ = View::kDefault;
    // -1 = auto-center the detail face's zoomed window on "now" (the
    // default); otherwise an explicit window_start in minutes-since-midnight
    // set by ScrollDetailWindow. Reset to -1 whenever the view is toggled
    // (see HandleInput's kBootClick case).
    int detail_window_start_override_ = -1;
#if CONFIG_BUSY_LIGHT_DEBUG_CYCLE
    DebugTier debug_tier_ = DebugTier::kFree;
    int debug_av_index_ = 0;  // index into kDebugAvStates (cam/presenting demo cycle)
#endif  // CONFIG_BUSY_LIGHT_DEBUG_CYCLE
    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;

    // Snapshot of the data as of the last time we actually redrew (not the
    // last time Update() was called — see the change-detection comment
    // there). Compared against the incoming status on each Update() to
    // decide whether a redraw is warranted at all.
    PresenceStatus last_evaluated_;
    bool has_rendered_once_ = false;
    // Time-derived pieces of the two faces' rendering that can change even
    // when last_evaluated_ is byte-identical to the incoming status — e.g.
    // "now" crossing a meeting's start/end minute, or the 5pm tomorrow-line
    // cutoff. See ComputeTimeDerivedSignature() in the .cc file.
    int32_t last_active_start_ = -1;
    int32_t last_active_end_ = -1;
    int32_t last_next_start_ = -1;
    int32_t last_past_event_count_ = -1;
    bool last_tomorrow_banner_visible_ = false;
    bool pending_visible_change_ = true;
    Rect pending_dirty_rect_{0, 0, 0, 0};  // {0,0,0,0} == full screen
};

}  // namespace rawdraw

#endif  // RAWDRAW_BUSY_LIGHT_RENDERER_H
