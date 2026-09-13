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
    BusyLightRenderer();
    ~BusyLightRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Data interface
    void Update(const PresenceStatus& status);

private:
    PresenceStatus current_;
    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_BUSY_LIGHT_RENDERER_H
