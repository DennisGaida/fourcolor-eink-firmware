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

    // Debug/demo status override for the header (word/swatch/band/human
    // line) — cycled by UP click. Independent of the real calendar so you
    // can preview every tier without editing mock data.
    enum class DebugTier {
        kFree,
        kInternal,
        kLeadership,
        kCustomer,
    };

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

    PresenceStatus current_;
    View view_ = View::kDefault;
    DebugTier debug_tier_ = DebugTier::kFree;
    int debug_av_index_ = 0;  // index into kDebugAvStates (cam/presenting demo cycle)
    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_BUSY_LIGHT_RENDERER_H
