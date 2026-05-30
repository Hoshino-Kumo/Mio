# Compact WiFi Board Bring-up Notes

This note captures the current known-good state for the `bread-compact-wifi`
board so the project can be resumed from another computer.

Functional checkpoint before this note: `a3c624b feat: update compact wifi display and audio handling`.

## Repository

```powershell
git clone https://github.com/Hoshino-Kumo/Mio.git
cd Mio
git pull origin main
```

Build in the existing ESP-IDF environment:

```powershell
& 'C:\Espressif\tools\ninja\1.12.1\ninja.EXE' -C build
```

If the build directory is missing or stale, use the ESP-IDF extension build task,
or run the normal `idf.py` build command after exporting the correct IDF
environment for the local machine.

## Board

Target board directory:

```text
main/boards/bread-compact-wifi
```

Current key configuration is in:

```text
main/boards/bread-compact-wifi/config.h
```

## Audio Hardware

Codec and amplifier:

- Codec: ES8311
- Speaker amp: NS4150B
- NS4150B input resistor currently tested as `100k`
- ES8311 soldering issue was fixed in hardware
- With the solder issue fixed, microphone samples are no longer permanently zero

Current audio pins:

| Signal | GPIO |
| --- | --- |
| I2S MCLK | GPIO16 |
| I2S BCLK | GPIO15 |
| I2S WS | GPIO6 |
| I2S DOUT | GPIO5 |
| I2S DIN | GPIO7 |
| ES8311 I2C SDA | GPIO18 |
| ES8311 I2C SCL | GPIO17 |

Current audio parameters:

| Parameter | Value |
| --- | --- |
| Input sample rate | 48000 |
| Output sample rate | 48000 |
| ES8311 input gain default | 30 dB |
| PA voltage model | 4.5 V |
| DAC voltage model | 3.3 V |
| PA gain compensation | 0.0 dB |

Notes:

- `AUDIO_CODEC_PA_GAIN_DB` is intentionally `0.0f` for the current 100k
  NS4150B input resistor state.
- If the speaker is still too quiet after using 100k, first adjust ES8311
  output/software volume before changing the PA compensation again.
- If wake word stops responding and logs show `peak=0 avg_abs=0`, check ES8311
  soldering, mic bias/input path, and I2S DIN before changing software.

## Display Hardware

Panel:

- Driver IC: ST7789V
- Interface: SPI
- Color: RGB565
- Logical UI orientation: landscape
- Logical resolution in software: 320x240

Current LCD pins:

| Signal | GPIO |
| --- | --- |
| SCLK | GPIO12 |
| MOSI | GPIO11 |
| CS | GPIO10 |
| DC | GPIO9 |
| RST | GPIO8 |
| Backlight | GPIO4 |

Current display parameters:

| Parameter | Value |
| --- | --- |
| SPI host | SPI2_HOST |
| Pixel clock | 20 MHz |
| Backlight mode | PWM |
| Backlight PWM frequency | 40 kHz |
| Default brightness | 80 |
| `DISPLAY_SWAP_XY` | true |
| `DISPLAY_MIRROR_X` | false |
| `DISPLAY_MIRROR_Y` | true |
| `DISPLAY_INVERT_COLOR` | false |

Hardware bring-up notes:

- The display backlight was not dead. The earlier no-current test was caused by
  enamel wire insulation under the power clip.
- The first board had an FPC connector mismatch: the PCB was designed for
  top-contact, but the installed connector was bottom-contact.
- If the screen is mirrored or upside down after moving to another build, check
  `DISPLAY_SWAP_XY`, `DISPLAY_MIRROR_X`, and `DISPLAY_MIRROR_Y`.
- If colors are inverted, check `DISPLAY_INVERT_COLOR`.

## Current UI

The LCD UI is currently a full-screen robot face:

- WiFi status icon at top-left
- Time at top-right
- Bottom subtitle/status line uses scrolling text
- No notification bubble for the normal bottom subtitle
- Background and face are flat colors to avoid visible banding
- The extra highlight dot on the face is hidden

Relevant implementation files:

```text
main/display/lcd_display.cc
main/display/lcd_display.h
main/boards/bread-compact-wifi/compact_wifi_board.cc
```

## Playback Behavior

The audio/application flow was adjusted so TTS playback is allowed to drain
before music playback or sleep-like idle transitions start.

Relevant implementation files:

```text
main/audio/audio_service.cc
main/audio/audio_service.h
main/application.cc
main/application.h
```

## Buttons

| Function | GPIO |
| --- | --- |
| Boot / flash mode | GPIO0 |
| Volume up | GPIO40 |
| Volume down | GPIO39 |

To enter ESP32-S3 download mode manually, hold GPIO0 low while resetting or
powering the chip.

## Known Local Issues

- `ClearCommError PermissionError(13)` from `idf_monitor.py` or `esptool.py`
  usually means the serial port is occupied, the monitor still owns it, or the
  USB serial driver is in a bad state. Close other monitors and replug the board.
- Windows may show CRLF/LF warnings or line-ending-only changes. At the time
  this note was written, `main/audio/audio_codec.cc` could appear modified
  locally without meaningful source changes.
- The latest verified build before this note passed with:

```powershell
& 'C:\Espressif\tools\ninja\1.12.1\ninja.EXE' -C build
```
