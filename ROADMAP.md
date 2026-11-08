# Roadmap

Tracks known issues and planned work for Liner. Security items come from a review of the v1.5.0 source and release binary on 2026-10-24.

**Priority:** P0 = fix before the next release · P1 = next release · P2 = soon · P3 = nice to have

## Security

### P0 · Toolchain is end-of-life and carries a remotely reachable CVE

- [x] Pin the platform to `platformio/espressif32 @ 8.0.0` (Arduino-ESP32 3.3.12, ESP-IDF 5.x) — v1.7.0
- [x] Port to Arduino-ESP32 3.x APIs and confirm M5Unified/M5GFX still detect the PaperColor panel — v1.7.0
- [x] Verify on hardware: boot, Wi-Fi, portal page, Tidal art (v1.7.0)
- [ ] Verify on hardware: setup AP, OTA, buttons, screensaver
- [ ] Cut a release from the new toolchain

The v1.5.0 binary was built on Arduino-ESP32 2.0.17, which uses ESP-IDF v4.4.7 and mbedTLS 2.28.7. ESP-IDF v4.4 reached end of life in July 2024, and mbedTLS 2.28 is also end of life, so neither gets security fixes.

| ID | Component | Affects Liner because | Fixed in |
|---|---|---|---|
| CVE-2026-42854 (critical) | Arduino `WebServer` multipart parser | The settings portal runs `WebServer`, and the parser runs on any POST before routing | Arduino-ESP32 3.3.8 |
| CVE-2026-45160 (medium) | ESP-IDF lwIP DHCP server | The DHCP server runs while the `Liner-Setup` AP is up. v4.4 is EOL and wasn't assessed, but it ships the same parser | ESP-IDF 5.x patch releases |
| CVE-2025-27809, CVE-2025-27810 and others | mbedTLS 2.28.7 | The album-art fetch uses TLS | mbedTLS 3.6 (via ESP-IDF 5.x) |

Advisories reviewed and **not** applicable: Bluetooth (BT unused), the ESP-IDF hardware JPEG decoder (Liner uses M5GFX's software decoder), WebServer `authenticate()` / `serveStatic()` (unused), NetBIOS and WPS (unused).

### P1 · Settings portal accepts cross-site requests

- [x] Every settings POST requires an `X-Liner` header, which cross-site pages can't send without a CORS preflight the device never answers — v1.8.0

Any web page open in a browser on the same LAN can submit the settings form and change the Wi-Fi network, Volumio host or OTA toggle.

### P1 · OTA is unauthenticated and on by default

- [ ] Set an OTA password with `ArduinoOTA.setPassword()`, generated per device and shown in the portal
- [ ] Or default OTA to **off** and let the user opt in
- [x] The OTA switch now fully stops the OTA listener and its mDNS advert (before, it only skipped `handle()` in the main loop) — v1.8.0

### P1 · HTML escaping misses single quotes

- [x] Replaced by the v1.8.0 dashboard: device data (SSIDs, Volumio host, track info, art URL) arrives as JSON and is inserted with `textContent`, never parsed as HTML

Values such as a nearby network's name or Volumio's album-art path end up inside single-quoted attributes, so they can break out of them.

### P2 · HTTPS certificates aren't verified

- [ ] Use the bundled CA store (`setCACertBundle`) for the wsrv.nl album-art fetch instead of the default insecure mode
- [x] Cap the album-art `Content-Length` before `ps_malloc()` — v1.6.0

`HTTPClient` without a CA certificate calls `setInsecure()`, so any certificate is accepted.

### P2 · Setup access point is open

- [ ] Give `Liner-Setup` a per-device WPA2 password and show it on the e-paper screen next to the AP name

### P2 · Builds aren't reproducible

- [x] Pin exact versions for the platform, M5Unified and ArduinoJson in `platformio.ini` — v1.7.0
- [ ] Add a GitHub Actions build that produces the release `.bin`, so the toolchain in each release is recorded

### P3 · Hardening

- [ ] Enable NVS encryption so a physically extracted flash dump doesn't expose the Wi-Fi password
- [ ] Add a `SECURITY.md` with a private reporting channel (GitHub private vulnerability reporting)
- [ ] Re-check Espressif and Arduino-ESP32 advisories on each release

## Features and known limitations

- [x] Optional dashboard password (digest auth, 5 s top-button hold removes it) — v1.8.0
- [x] QR codes on the panel for setup and for "can't reach Volumio" — v1.8.0
- [x] Responsive dashboard: bottom tabs on phones, header tabs and two columns on tablets and desktops — v1.8.0
- [ ] Verify on hardware: setup-mode QR screen, captive portal pop-up, Wi-Fi retry from setup mode, 5 s button hold

- [ ] Verify album art for local files (`mpd`) and Spotify (`spop`/`volspotconnect2`). Only Tidal is exercised today
- [ ] Make flashing and booting over USB work without manual button presses
