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
};

}  // namespace rawdraw

#endif  // RAWDRAW_BUSY_LIGHT_RENDERER_H
