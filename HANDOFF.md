# Mio Handoff

## Project

- Main workspace: `H:\OpenCodeProject\Mio`
- Remote: `https://github.com/Hoshino-Kumo/Mio.git`
- Active branch: `main`
- Target hardware: ESP32-S3, `bread-compact-wifi`, ES8311 audio module.

## Current Hardware Configuration

- Board code kept: `main/boards/bread-compact-wifi` and `main/boards/common`
- Build is fixed to `bread-compact-wifi` in `main/CMakeLists.txt`
- Audio sample rate is currently 48 kHz for both input and output:
  - `AUDIO_INPUT_SAMPLE_RATE 48000`
  - `AUDIO_OUTPUT_SAMPLE_RATE 48000`
- ES8311 pins:
  - SDA `GPIO18`
  - SCL `GPIO17`
  - MCLK `GPIO16`
  - BCLK/SCLK `GPIO15`
  - WS/LRCK `GPIO6`
  - DOUT to module `GPIO5`
  - DIN from module `GPIO7`
- Microphone input is intentionally disabled for now:
  - `AUDIO_MICROPHONE_ENABLED 0`
  - Re-enable only after the microphone path is physically installed and validated.

## Recent Important Changes

- `e4f3912` disables the microphone input path at the audio service boundary and fixes ES8311 read failure handling.
- `84d6581` disables idle AFE wake word detection to avoid boot-time AFE FEED ringbuffer overflow.
- `b7667cf` disables unstable touch-button press/release listening callbacks.
- `9efa34d` keeps only the Mio board and fixes media playback state so streaming playback is not considered complete until the playback queue drains.

## Known Current Behavior

- Wake word detection is intentionally disabled in idle mode.
- Touch button no longer starts/stops listening.
- Manual listening should still be entered through the remaining button/chat control path.
- Streaming audio playback should remain in speaking/media state until queued audio has played or the stream is aborted.

## Common Commands

```powershell
cd H:\OpenCodeProject\Mio
git status
git pull origin main
idf.py set-target esp32s3
idf.py build
```

## Next Debug Targets

- If `AFE(FEED) is full` still appears with `AUDIO_MICROPHONE_ENABLED 0`, search for any code path that feeds AFE without going through `AudioService::ReadAudioData()`.
- After installing the microphone, set `AUDIO_MICROPHONE_ENABLED` to `1`, then validate ES8311 input before re-enabling idle wake word detection.
- If streaming playback still cannot be interrupted, inspect `Application::AbortSpeaking()`, `media_streaming_`, and `AudioService::ResetDecoder()`.
