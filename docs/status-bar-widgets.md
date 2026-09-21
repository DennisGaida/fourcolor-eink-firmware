# Status bar widgets

The shared OS status bar (`RawDrawUiManager::DrawStatusBar()`,
`firmware/main/ui/rawdraw_ui_manager.cc`) normally only shows chrome that's
common to every page: Wi-Fi/server/Bluetooth indicators, date, clock,
battery, and the active page's title. This document describes the extension
point that lets an **inactive** page/module draw a small always-on glimpse
of its own state into that shared bar — e.g. the current weather icon +
temperature while looking at BusyLight, or the camera state + MEETING/FREE
word while looking at Weather (or any other page).

## Why this exists

Every module's renderer instance is owned for the lifetime of the app by
`RawDrawUiManager` (`weather_renderer_`, `busy_light_renderer_`, etc.), and
each keeps receiving fresh data pushes regardless of which page is actually
on screen — e.g. `UpdateWeatherData()`/`UpdatePresenceStatus()` call
straight into the corresponding renderer's `Update()` even when that page
isn't active. So an inactive module's renderer *already* holds live,
up-to-date state; the only missing piece was a place to draw a
few-pixels-wide summary of it.

## The extension point

`PageRenderer` (`firmware/main/ui/renderers/rawdraw/page_renderer.h`) has an
optional virtual, defaulting to a no-op:

```cpp
// Optional: contribute a small always-on summary to the shared status bar
// while this page is NOT the active/rendered one.
//
// Implementations should draw right-aligned, ending at right_edge_x,
// vertically centered on center_y, using no more than max_w pixels, and
// must draw nothing (returning 0) if they have no useful content or don't
// fit within max_w.
//
// @return width in pixels actually consumed (0 if nothing was drawn)
virtual int RenderStatusBarWidget(uint8_t* fb, int width, int right_edge_x,
                                  int center_y, int max_w) {
    return 0;
}
```

`RawDrawUiManager::DrawStatusBar()` calls this on every module renderer
**except** the currently-active one, right after laying out the clock and
battery (i.e. in the gap between the clock/battery and the centered page
title):

```cpp
// rawdraw_ui_manager.cc, inside DrawStatusBar()
constexpr int kStatusWidgetMaxW = 70;
constexpr int kStatusWidgetGap = 10;
int status_widget_budget = kStatusWidgetMaxW;
if (weather_renderer_ && current_page_ != RawDrawPageId::Weather) {
    const int widget_w = weather_renderer_->RenderStatusBarWidget(
        fb, width, right_x, center_y, status_widget_budget);
    if (widget_w > 0) right_x -= widget_w + kStatusWidgetGap;
}
if (busy_light_renderer_ && current_page_ != RawDrawPageId::BusyLight) {
    const int widget_w = busy_light_renderer_->RenderStatusBarWidget(
        fb, width, right_x, center_y, status_widget_budget);
    if (widget_w > 0) right_x -= widget_w + kStatusWidgetGap;
}
```

Each widget that actually draws something shrinks `right_x`, which in turn
shrinks `right_safe` (the boundary the centered title text is fitted/clamped
against) — so widgets and the title never overlap, and multiple widgets can
coexist by chaining right-to-left. A widget that returns 0 (no data yet, or
doesn't fit in the budget) is simply skipped with no visual trace.

### Contract for implementers

- **Draw right-aligned**: your rightmost pixel must land at `right_edge_x`.
  The bar hands you the current right edge of the "free" zone, not a fixed
  slot — the previous widget in the chain may have already consumed some of
  it.
- **Respect `max_w`**: measure your content's total width first; if it
  doesn't fit, draw nothing and return `0` rather than clipping or
  overflowing into the title.
- **Return the width you actually consumed** (0 if nothing was drawn) so the
  caller can shift the next slot over correctly.
- **Reuse the module's own theme/color logic.** Widgets call
  `ThemeManager::Get()` directly (same as the module's own `Render()`), so
  colors stay in sync with whatever the page itself would show — no color
  state needs to be threaded through the interface.
- **Fail closed.** Guard on `fb` and on having real data (`has_data_`/
  `current_.valid`) — a module with no data yet should contribute nothing,
  not a placeholder.

## Weather's widget

`WeatherRenderer::RenderStatusBarWidget()`
(`firmware/main/ui/renderers/rawdraw/weather_renderer.cc`) draws, right to
left: `"NN°"` (in `font_`, the same 16px `SourceHanSansSC_Regular_slim` used
for the bar's date/title text) preceded by a small weather icon
(`weather_icons_v2_16`, the same icon font used for the alert badge —
selected via the existing `IconGlyphFor(WeatherIconForCode(...))`/
`DrawWeatherIcon()` helpers, so the glyph always matches what the Weather
page's own hero icon would show). Returns `0` if `has_data_` is false (no
successful fetch yet) or if the icon+temp combo doesn't fit in the budget.

## BusyLight's widget

`BusyLightRenderer::RenderStatusBarWidget()`
(`firmware/main/ui/renderers/rawdraw/busy_light_renderer.cc`) draws, right
to left: the bold "MEETING"/"FREE" word (`font_`, faux-bold via
`DrawTextBold`'s 1px double-strike — same treatment as the tier-carrying
segment of the default face's human line) preceded by a compact camera icon
(`DrawStatusBarCamIcon()`, a scaled-down, label-less version of the default
face's `DrawCameraGlyph()` — same rounded body/lens/off-slash shape, just
14x10px instead of 20x14px).

Both the word and the camera icon reuse the exact same effective-status
derivation as the default face's header
(`ComputeEffectiveStatus()`/`ActiveEvent()`, including the "solo calendar
blocks read as free" rule and the `CONFIG_BUSY_LIGHT_DEBUG_CYCLE` override
when that Kconfig option is enabled), so the widget never disagrees with
what the BusyLight page itself would show right now:

- **Word color**: red when `EffectiveStatus::band_elevated` is true
  (leadership/customer tier, an ad-hoc call/webcam/screen-share with
  nothing on the calendar, or presenting) — same rule that colors the
  default face's top status band — otherwise the theme's primary text
  color.
- **Camera icon color**: red whenever `webcam_active` is true (matching
  `DrawCameraGlyph`'s "CAM ON is always red" rule everywhere it appears),
  otherwise the theme's secondary text color; a diagonal slash overlays the
  icon when the camera is off.

Returns `0` if `current_.valid` is false (no successful presence fetch yet)
or if the icon+word combo doesn't fit in the budget.

## Adding a widget to another module

1. Override `RenderStatusBarWidget()` in the module's `PageRenderer`
   subclass, following the contract above (right-aligned at `right_edge_x`,
   budget-checked against `max_w`, return consumed width or `0`).
2. Add a call to it in `RawDrawUiManager::DrawStatusBar()`, guarded by
   `current_page_ != RawDrawPageId::<YourPage>`, chained after the existing
   calls so it inherits whatever `right_x` the earlier widgets left behind.
3. No changes needed to the module's `Update()`/data-push path — it already
   keeps its renderer's state current regardless of which page is active.
