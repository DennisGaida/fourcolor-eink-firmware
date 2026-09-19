# Power optimizations — follow-up tracking

Working notes for the `optimizations` branch. Items here are either
unverified-on-hardware risks or changes intentionally deferred.

## To verify on device

- **WiFi power-save: BALANCED after connect settle (unverified)**
  `firmware/main/application.cc` (`ArmWifiPowerSaveSettleTimer` /
  `ApplySteadyStateWifiPowerSave`) switches WiFi from `WIFI_PS_NONE` to
  `WIFI_PS_MIN_MODEM` (BALANCED) 10s after a WiFi connect, to save power
  during steady-state polling. The connect-time default was previously
  changed to `WIFI_PS_NONE` specifically because modem sleep caused slow
  TCP handshakes and a high HTTP failure rate (see comment in
  `wifi_station.cc` around `esp_wifi_set_ps(WIFI_PS_NONE)`).
  BALANCED is only applied *after* the handshake/DHCP phase, but this is
  still speculative — it has not been validated against real presence
  API / LAN server traffic.
  - **Action**: after flashing, leave the device connected for ~60
    minutes with normal presence/calendar polling (90s / 20min) running,
    and check device logs (`idf.py monitor` or the `/screenshot` +
    on-device log renderer) for HTTP timeouts, retries, or failures from
    `presence_api.cc`, `weather_api.cc`, or `holiday_fetcher.cc`.
  - **If failures reappear**: revert `ApplySteadyStateWifiPowerSave` to a
    no-op (or gate it behind a Settings toggle defaulting off) and leave
    WiFi at `WIFI_PS_NONE` for the whole session, matching the original
    fix.

## Deferred (not implemented in this pass)

- **CPU dynamic frequency scaling / `CONFIG_PM_ENABLE`**: CPU is
  currently pinned at 240MHz (`sdkconfig.defaults.esp32s3`). Enabling
  automatic light sleep / DFS via `esp_pm_configure` could save
  meaningful power but risks timing-sensitive SPI/e-ink and WiFi driver
  behavior — needs dedicated hardware testing, not done opportunistically.
- **Light sleep between UI ticks**: `SleepManager`
  (`firmware/main/common/sleep_manager.h/.cc`) has scaffolding
  (`PrepareForLightSleep`, `ScheduleTimerWakeup`) but nothing calls
  `esp_light_sleep_start()` yet. Worth wiring up once BALANCED WiFi PS is
  confirmed stable.
