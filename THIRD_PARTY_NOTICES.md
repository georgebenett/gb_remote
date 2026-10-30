# Third-party notices

| Component | Where | License |
|---|---|---|
| ESP-IDF BLE SPP client example (basis of `firmware/main/ble.c`) | Espressif Systems | Unlicense OR CC0-1.0 |
| LVGL 9.1.0 | component manager (`firmware/dependencies.lock`) | MIT |
| esp_lvgl_port 2.4.4 | component manager | Apache-2.0 |
| ESP-IDF | build toolchain | Apache-2.0 |
| Bebas Neue font (Dharma Type) | `UI/eez_*/assets/BebasNeue-Regular.ttf`, rendered into `firmware/main/ui_*/ui_font_bebas*.c` | SIL Open Font License 1.1, see `UI/eez_*/assets/OFL-BebasNeue.txt` |
| Satoshi font (Indian Type Foundry) | rendered into `firmware/main/ui_*/ui_font_satoshi*.c` | ITF Free Font License |

The Satoshi font files are not in this repository: their license does not
allow redistributing them. To change fonts in EEZ Studio, download Satoshi from
<https://www.fontshare.com/fonts/satoshi> and place `Satoshi-Regular.otf` and
`Satoshi-Variable.ttf` in `UI/eez_lite/assets/` and `UI/eez_dual_throttle/assets/`.

The images in `UI/eez_*/assets/` are original artwork for this project and
fall under the project license.
