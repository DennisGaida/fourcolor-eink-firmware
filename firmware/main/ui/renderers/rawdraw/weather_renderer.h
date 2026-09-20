/**
 * @file weather_renderer.h
 * @brief Weather page renderer for rawdraw mode
 *
 * Rawdraw weather page renderer for 400x300 e-paper.
 */

#ifndef RAWDRAW_WEATHER_RENDERER_H
#define RAWDRAW_WEATHER_RENDERER_H

#include "common/weather_api.h"
#include "page_renderer.h"
#include "rawdraw/style.h"
#include <string>

namespace rawdraw {

class WeatherRenderer : public PageRenderer {
public:
    WeatherRenderer();
    ~WeatherRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Data interface
    // Pushes freshly fetched data into the renderer (see weather_api.h).
    void Update(const WeatherData& data);

private:
    WeatherData current_data_;
    bool has_data_ = false;
    const lv_font_t* font_ = nullptr;        // SourceHanSansSC_Regular_slim (16px)
    const lv_font_t* title_font_ = nullptr;  // SourceHanSansSC_Medium_slim (24px)
};

}  // namespace rawdraw

#endif  // RAWDRAW_WEATHER_RENDERER_H
