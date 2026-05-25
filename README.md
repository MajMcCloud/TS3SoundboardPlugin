# TS3SoundboardPlugin

A TeamSpeak 3 client plugin written in **C++17** that acts as the audio receiver for the **TS3 Soundboard** system. It works together with the [TS3Soundboard](../TS3Soundboard) Stream Deck plugin, which sends playback commands via a named pipe. The plugin injects audio files directly into the TeamSpeak microphone stream so that other participants hear the sound, while optionally playing it back locally through the speakers as well.

---

## Features

- **Microphone injection** – plays audio files into the TS3 capture (microphone) path so remote participants hear the sound.
- **Local playback** – simultaneously plays audio back through the local speakers at an independently configurable volume.
- **Multi-format audio** – supports WAV (PCM), MP3, OGG Vorbis and FLAC files.
- **Audio mixing** – multiple sounds can be active at the same time; all are mixed together in real time.
- **Named pipe server** – listens on `\\.\pipe\TS3Soundboard` for JSON commands from the Stream Deck plugin.
- **Settings dialog** – a native Win32 settings dialog accessible from the TS3 plugin menu lets you configure base volumes and logging.
- **Persistent configuration** – settings are stored in `<TS3ConfigPath>/ts3soundboard.json`.
- **Localization** – UI strings are loaded from JSON locale files at runtime; the language is derived from the Windows system locale.

---

## Architecture

```
Stream Deck Plugin (C#)
		│
		│  Named Pipe  \\.\pipe\TS3Soundboard
		│  JSON command: { "filePath": "...", "volume": 100 }
		▼
TS3SoundboardPlugin (this project, C++ DLL)
		│
		├── PipeServer        – receives commands, dispatches to AudioMixer
		├── AudioMixer        – decodes audio, mixes into TS3 audio callbacks
		├── Config            – loads/saves ts3soundboard.json
		├── SettingsDialog    – Win32 dialog for plugin settings
		└── plugin_locale.h  – runtime i18n via locales/*.json
```

### Key source files

| File | Description |
|---|---|
| `src/plugin.cpp` / `plugin.h` | TS3 SDK entry points, audio callbacks, CT management |
| `src/audio_mixer.cpp` / `audio_mixer.h` | Audio decoding, resampling to 48 kHz / mono, real-time mixing |
| `src/pipe_server.cpp` / `pipe_server.h` | Named pipe server (background thread, JSON protocol) |
| `src/config.cpp` / `config.h` | Plugin configuration (load/save JSON) |
| `src/settings_dialog.cpp` / `settings_dialog.h` | Win32 settings dialog |
| `src/plugin_locale.h` | Header-only locale singleton |
| `src/vendor/minimp3.h` | Header-only MP3 decoder |
| `src/vendor/stb_vorbis.h` | Header-only OGG Vorbis decoder |
| `src/vendor/dr_flac.h` | Header-only FLAC decoder |
| `locales/*.json` | Localization strings |

---

## Named Pipe Protocol

The pipe operates in duplex mode. Commands and responses are newline-terminated JSON strings.

**Play command (Stream Deck → Plugin):**
```json
{ "filePath": "C:\\sounds\\test.mp3", "volume": 100 }
```

**Stop-all command:**
```json
{ "stopAll": true }
```

**Response (Plugin → Stream Deck):**
```json
{ "status": "ok" }
{ "status": "error", "message": "File not found" }
```

---

## Supported Audio Formats

| Format | Decoder |
|---|---|
| WAV (PCM) | Built-in chunk parser |
| MP3 | `vendor/minimp3.h` (header-only) |
| OGG Vorbis | `vendor/stb_vorbis.h` (header-only) |
| FLAC | `vendor/dr_flac.h` (header-only) |

All formats are decoded and resampled to **48 kHz / mono** before mixing.

---

## Configuration

Settings are stored in `<TS3ConfigPath>/ts3soundboard.json`:

| Field | Type | Default | Description |
|---|---|---|---|
| `baseVolumeRemote` | float (0.0 – 1.0) | `0.8` | Base volume for the microphone (remote) path |
| `baseVolumeLocal` | float (0.0 – 1.0) | `0.8` | Base volume for local speaker playback |
| `enableLogging` | bool | `false` | Print debug messages to the TS3 console |

The per-button volume offset sent with each play command is added on top of the base volume.

---

## Localization

UI strings are loaded from `ts3_soundboard/locales/<lang>.json` at plugin startup. The language is resolved as follows:

1. Full Windows locale name (e.g. `de-DE.json`)
2. Language prefix (e.g. `de.json`)
3. Fallback: `en.json`

### Supported languages

`de`, `en`, `es`, `fr`, `it`, `ja`, `ko`, `pl`, `pt`, `ro`, `ru`, `uk`, `zh-CN`, `zh-TW`, `bg`

---

## Building

### Prerequisites

- **MSVC** (Visual Studio 2019 or later, x64 toolchain)
- **CMake 3.16+**
- **Ninja** build system
- TS3 Client Plugin SDK 26 located at `ts3client-pluginsdk-26/` inside this folder

### Build (recommended)

Use the provided script from the repository root to set up the correct x64 environment:

```cmd
build_x64_plugin.cmd
```

This initializes the Visual Studio x64 developer environment and builds the `x64-debug` CMake preset.

### Manual build

```cmd
cmake --preset x64-debug -DPLUGIN_VERSION=1.0.0
cmake --build --preset x64-debug
```

> **Important:** Always build with the x64 toolchain. TeamSpeak 3 is a 64-bit application and will not load a 32-bit DLL.

### Output

| Architecture | Output file |
|---|---|
| x64 | `ts3_soundboard_win64.dll` |
| x86 | `ts3_soundboard_win32.dll` |

The architecture suffix is determined automatically via `CMAKE_SIZEOF_VOID_P`.

After a successful build, the locale files are automatically copied next to the DLL:

```
out/build/x64-debug/
├── ts3_soundboard_win64.dll
└── ts3_soundboard/
	└── locales/
		├── en.json
		├── de.json
		└── ...
```

---

## Installation

1. Build the plugin (see above) or obtain a release package.
2. Copy `ts3_soundboard_win64.dll` and the `ts3_soundboard/` folder to:
   ```
   %APPDATA%\Elgato\StreamDeck\Plugins\uk.zevedei.ts3soundboard.sdPlugin\
   ```
3. Restart TeamSpeak 3. The plugin appears under **Plugins → Installed**.

> For a fully packaged installation, use `publish.ps1` from the repository root, which creates a `.ts3_plugin` installer package.

---

## TS3 SDK Callbacks

| Callback | Purpose |
|---|---|
| `ts3plugin_onConnectStatusChangeEvent` | Detects `STATUS_CONNECTION_ESTABLISHED`, sets the active server handle |
| `ts3plugin_onEditCapturedVoiceDataEvent` | Mixes audio into the microphone (capture) path |
| `ts3plugin_onEditMixedPlaybackVoiceDataEvent` | Mixes audio into the local speaker (playback) path |

Continuous Transmission (CT) is temporarily activated during playback so that `onEditCapturedVoiceDataEvent` is triggered regardless of PTT/VAD settings. CT is restored to its original state once playback finishes.
