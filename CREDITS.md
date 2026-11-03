# Credits

The Harkenn speaker firmware is built on these projects. Thank you to all of them.

## Libraries compiled into the firmware

| Project | Used for | Licence |
|---|---|---|
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) (Espressif), via the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform | Framework: Wi-Fi, HTTP, NVS, mDNS, OTA | LGPL-2.1 |
| [ESP-IDF](https://github.com/espressif/esp-idf) (Espressif), including FreeRTOS, lwIP and Mbed TLS | Underlying SDK: I2S, stream buffers, TLS, SHA-256, the web page's PBKDF2 and the relay's AES-GCM | Apache-2.0 (FreeRTOS: MIT; lwIP: BSD-3-Clause) |
| [LVGL](https://github.com/lvgl/lvgl) | Screen UI | MIT |
| [Montserrat](https://github.com/JulietaUla/Montserrat) (bundled with LVGL) | Font | SIL OFL 1.1 |
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) | Display (GC9A01) and touch (CST816) drivers | MIT and BSD-2-Clause |
| [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets) (Links2004) | WebSocket connection to the server | LGPL-2.1 |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) (Benoît Blanchon) | JSON | MIT |
| [TensorFlow Lite Micro](https://github.com/tensorflow/tflite-micro), as Espressif's [esp-tflite-micro](https://github.com/espressif/esp-tflite-micro) (in `lib/tflm`) | Runs the wake word model | Apache-2.0 |
| TensorFlow's micro audio frontend ([tensorflow/lite/experimental/microfrontend](https://github.com/tensorflow/tensorflow/tree/master/tensorflow/lite/experimental/microfrontend)) | Wake word audio features | Apache-2.0 |
| [kissfft](https://github.com/mborgerding/kissfft) (Mark Borgerding), bundled with esp-tflite-micro | FFT for those features | BSD-3-Clause |
| [esp-nn](https://github.com/espressif/esp-nn) (Espressif, in `lib/esp-nn`) | ESP32-S3 vector kernels for the model | Apache-2.0 |
| [FlatBuffers](https://github.com/google/flatbuffers), [gemmlowp](https://github.com/google/gemmlowp), [ruy](https://github.com/google/ruy) (Google), bundled with esp-tflite-micro | Model format and maths helpers for TFLM | Apache-2.0 |
| [microWakeWord](https://github.com/kahrendt/microWakeWord) (Kevin Ahrendt) | The wake word models' design and feature settings the speaker follows, and the "stop" model it listens for while a timer or alarm rings; the models themselves come from Harkenn | Apache-2.0 |
| [ISRG Root X1 and X2](https://letsencrypt.org/certificates/) (Let's Encrypt), [GTS Root R1, R3, R4](https://pki.goog/repository/) (Google Trust Services), [SSL.com Root CAs](https://www.ssl.com/repository/) (RSA, ECC) | Root certificates in `src/ca_certs.h`, for servers on https and the relay behind Cloudflare | public |

The firmware's full source is in this repository, so the LGPL libraries can be rebuilt and relinked from it.

## Hardware knowledge

- Pin assignments, the display and touch setup, and the audio codec wiring of the Spotpear ESP32-S3 1.28" BOX come
  from its board definition in [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (MIT), the speaker's original
  firmware.
- The ES8311 codec setup in `src/audio.cpp` follows the register sequence and clock coefficients of Espressif's
  ES8311 driver ([esp-adf](https://github.com/espressif/esp-adf) / [esp_codec_dev](https://github.com/espressif/esp-adf/tree/master/components/esp_codec_dev), Apache-2.0).

## Tools

[PlatformIO](https://platformio.org) (Apache-2.0) builds the firmware; [esptool](https://github.com/espressif/esptool)
(GPL-2.0) flashes it.

## Formats and protocols

The KANI animation format and the device protocol are this project's own: see `app/kani.py` and `docs/PROTOCOL.md`
in [server-docker](https://github.com/harkenn-project/server-docker).
