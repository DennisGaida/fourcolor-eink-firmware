/**
 * @file page_renderer.h
 * @brief Rawdraw page renderer base class (no LVGL dependency)
 */

#ifndef RAWDRAW_PAGE_RENDERER_H
#define RAWDRAW_PAGE_RENDERER_H

#include "rawdraw/framebuffer.h"
#include "rawdraw/font_engine.h"
#include <functional>
#include <string>

namespace rawdraw {

/**
 * @brief Button event types
 */
struct ButtonEvent {
    enum Type {
        kUpClick,
        kDownClick,
        kUpDoubleClick,
        kDownDoubleClick,
        kUpLongPress,
        kDownLongPress,
        kBootClick,
        kBootDoubleClick,
        kBootLongPress,
    };
    Type type;
};

/**
 * @brief Page renderer base class for rawdraw mode
 *
 * All page renderers must implement this interface.
 * Renders directly to 1bpp framebuffer, no LVGL dependency.
 */
class PageRenderer {
public:
    virtual ~PageRenderer() = default;

    /**
     * @brief Initialize page resources
     *
     * Called once when page becomes active. Set up fonts, initial state.
     */
    virtual void Init(int width, int height) = 0;

    /**
     * @brief Render page to framebuffer
     *
     * Called on each display update. Draw all page content.
     *
     * @param fb Framebuffer to render to
     * @param width Framebuffer width
     * @param height Framebuffer height
     */
    virtual void Render(uint8_t* fb, int width, int height) = 0;

    /**
     * @brief Handle button input
     *
     * @param event Button event
     * @return true if event was consumed
     */
    virtual bool HandleInput(const ButtonEvent& event) = 0;

    /**
     * @brief Get dirty rect for partial refresh
     *
     * @return Rect that needs refresh, or {0,0,0,0} for full refresh
     */
    virtual Rect GetDirtyRect() const { return {0, 0, 0, 0}; }

    /**
     * @brief Check if page needs full refresh
     *
     * @return true if full refresh needed (e.g., page switch)
     */
    virtual bool NeedsFullRefresh() const { return needs_full_refresh_; }

    /**
     * @brief Mark page as needing full refresh
     */
    void MarkFullRefresh() { needs_full_refresh_ = true; }

    /**
     * @brief Clear full refresh flag
     */
    void ClearFullRefreshFlag() { needs_full_refresh_ = false; }

    // Streaming support (for chat pages)
    virtual bool AppendText(const char* chunk) { (void)chunk; return false; }
    virtual void BeginStream() {}
    virtual void EndStream() {}

    /**
     * @brief Optional: contribute a small always-on summary to the shared
     * status bar while this page is NOT the active/rendered one (e.g. a
     * weather icon + temperature shown while BusyLight is active).
     *
     * Modules keep receiving data updates regardless of which page is
     * active, so this lets an inactive module surface a minimal glimpse of
     * its state in the space between the clock/battery and the page title.
     *
     * Implementations should draw right-aligned, ending at @p right_edge_x,
     * vertically centered on @p center_y, using no more than @p max_w
     * pixels, and must draw nothing (returning 0) if they have no useful
     * content or don't fit within @p max_w.
     *
     * @return width in pixels actually consumed (0 if nothing was drawn)
     */
    virtual int RenderStatusBarWidget(uint8_t* fb, int width, int right_edge_x,
                                      int center_y, int max_w) {
        (void)fb; (void)width; (void)right_edge_x; (void)center_y; (void)max_w;
        return 0;
    }

protected:
    int width_ = 0;
    int height_ = 0;
    bool needs_full_refresh_ = true;
};

}  // namespace rawdraw

#endif  // RAWDRAW_PAGE_RENDERER_H
