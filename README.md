# Nous

**Nous** is a personal fork of [microreader](https://github.com/CidVonHighwind/microreader/) by [CidVonHighwind](https://github.com/CidVonHighwind) — an open-source EPUB reader firmware for the [Xteink X4](https://xteink.com) e-ink device (ESP32-C3).

This fork extends the original with additional quality-of-life features while staying true to what makes microreader great: a focused, fast, no-nonsense reading experience.

Full credit and respect to the original project. This fork exists because of the excellent foundation it provides.

> Version 1.0.0 &nbsp;·&nbsp; Based on microreader 2.0-dev &nbsp;·&nbsp; GPL v2

---

## What's New

| Feature | Description |
|---------|-------------|
| **Convert All** | Batch-convert every un-converted EPUB from Settings, with per-book progress |
| **Converted Indicator** | Optional `*` prefix on converted book titles in the book list |
| **Reading Stats** | Per-book open count and total reading time, viewable from Reader Options |
| **Battery Display** | Three modes: icon only, number only, or both |
| **List Alignment** | Choose center, left, or right alignment for all list screens |
| **Hide Nav Arrows** | Toggle the navigation hint glyphs on/off |

| | | |
|---|---|---|
| ![Alignment](images/allign.png) | ![Convert All](images/convertall.png) | ![Stats](images/stats.png) |

---

## Installation

> [!WARNING]
> **Requires an unlocked Xteink X4.** Do not flash on a locked device.

Flash a `.bin` from the [Releases](../../releases) page using the [Crosspoint flash tool](https://crosspointreader.com/#flash-tools) (browser-based, no install needed), or with esptool:

```powershell
python -m esptool --chip esp32c3 --port COM5 --baud 921600 write_flash 0x0 nous.bin
```

Replace `COM5` with your port. Hold BOOT while connecting if the device doesn't enter flash mode automatically.

---

## Building

### ESP32 Firmware (PlatformIO)

Open the project in VS Code with the PlatformIO extension installed and click **Build**, or use the CLI:

```powershell
pio run
```

The output `.bin` will be at `.pio\build\esp32c3\firmware.bin`.

### Desktop Emulator

Runs the full UI in an SDL2 window — no hardware needed. Drop `.epub` files in `sd/`.

```powershell
cmake -B build/desktop-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ "-DCMAKE_POLICY_VERSION_MINIMUM:STRING=3.5" platforms/desktop
cmake --build build/desktop-debug --config Debug
.\build\desktop-debug\microreader_desktop.exe
```

---

## License

GPL v2 — see [LICENSE](LICENSE).

This project is a fork of [microreader](https://github.com/CidVonHighwind/microreader/) and inherits its GPL v2 license. All additions and modifications in this fork are released under the same terms.

---

## Original microreader README

Everything below is from the original [microreader](https://github.com/CidVonHighwind/microreader/) project.

---

# Microreader

An EPUB reader for the [Xteink X4](https://xteink.com), written from scratch.

Built around one idea: do EPUB rendering really well and nothing else. No Wi-Fi, no Bluetooth, no sync. Just put EPUBs on the SD card and read them.

The first time you open a book there's a single conversion pass to a compact binary format. After that — changing chapters, adjusting font size, tweaking settings — everything is instant.

**Features**

- EPUB rendering with proportional fonts, bold/italic, and inline images
- Hyphenation via the Liang/TeX algorithm (EN, DE, FR, ES, IT, NL, PT, PL, RU)
- Fully configurable reader: font, size, line spacing, margins, justification
- Multiple built-in and SD card fonts — swap at runtime without reflashing
- Table of contents navigation with chapter progress display
- Book position saved and restored automatically per book
- Customizable sleep screen with looping support
- Single-pass EPUB → `.mrb` conversion — fast cold open, instant everything after
- Licensed under GPL v2

**Demo Video**

[![Microreader demo](https://img.youtube.com/vi/KPsLV7BLEz0/0.jpg)](https://www.youtube.com/shorts/KPsLV7BLEz0)

https://www.youtube.com/shorts/KPsLV7BLEz0

## Installation

> [!WARNING]
> **Requires an unlocked Xteink X4.** Do not flash this on a locked device — you may be permanently stuck on that firmware.

Download the latest `.bin` from the **[Releases](https://github.com/CidVonHighwind/microreader/releases)** page.

Flash using the [Crosspoint flash tool](https://crosspointreader.com/#flash-tools) (browser-based, no install needed), or with [esptool](https://docs.espressif.com/projects/esptool/en/latest/) directly:

```powershell
python -m esptool --chip esp32c3 --port COM5 --baud 921600 write_flash 0x0 microreader.bin
```

Replace `COM5` with your device's port (`/dev/ttyUSB0` on Linux, `/dev/cu.usbserial-*` on macOS). Hold BOOT while connecting if the device doesn't enter flash mode automatically.

## Managing Content

Books (`.epub`) go anywhere on the SD card — the device scans recursively from the root. Fonts (`.mfb`) go in the `fonts/` folder. Sleep images go in the `.sleep/` folder.

You can copy files directly to the SD card, or transfer them over USB while the device is connected.

### Browser Manager

Open [microreader-manager](https://cidvonhighwind.github.io/microreader/) in Chrome, Edge, or Firefox (Web Serial API). Provides a file browser, EPUB/font/sleep-image upload, and auto-reconnects on page refresh.

![Microreader Manager](images/Microreader%20Manager.png)

Also includes a **Font Generator** for building `.mfb` fonts in the browser with live device preview, size presets, and script/range presets.

![Font Generator](images/Font%20Generator.png)

### Calibre Plugin

Install the plugin to send books directly from [Calibre](https://calibre-ebook.com):

1. In Calibre: **Preferences → Plugins → Load plugin from file** → select `tools/calibre-plugin/microreader.zip`
2. Restart Calibre.

The device is detected automatically. Books on the device show checkmarks in the library; you can send and delete books from the Device menu.

> Requires Calibre 5+ and the device connected over USB.

## Sleep Screen

The device shows an image when it enters deep sleep. Several images are built into the firmware; add your own by placing BMP files in the `.sleep/` folder on the SD card, or upload them via the browser manager.

**Supported formats:** 1 bpp monochrome, 4/8 bpp indexed, 16 bpp RGB565/BGR555, 24 bpp BGR, 32 bpp BGRA. Use 800×480 (landscape) or 480×800 for best quality — other sizes are scaled to fit.

The first time an image is shown it is converted and cached; subsequent sleeps load the cache directly. Clear the cache via **Settings → Clear Cache**.

Configure in **Settings → Sleep Image**: **Auto** cycles through all images in `.sleep/`; selecting a specific filename pins the device to that image.

## Building

### Prerequisites

| Tool | Purpose |
|------|---------|
| CMake 3.5+ | Build system |
| SDL2 | Desktop emulator |
| Python 3 | Tools and scripts |
| [PlatformIO](https://platformio.org/) | ESP32 firmware build and flash |

### Desktop (emulator)

```powershell
cmake -S platforms/desktop -B build/desktop-debug -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_POLICY_VERSION_MINIMUM:STRING=3.5"
cmake --build build/desktop-debug --config Debug
.\build\desktop-debug\Debug\microreader_desktop.exe
```

| | |
|---|---|
| ![Book list](images/main%20menu.png) | ![Reader](images/reader.png) |

![Reading options](images/reading%20options.png)

![Settings](images/settings.png)

### ESP32 (PlatformIO)

```powershell
$env:USERPROFILE\.platformio\penv\Scripts\pio.exe run -t upload
$env:USERPROFILE\.platformio\penv\Scripts\pio.exe device monitor --baud 115200
```

### Tests

```powershell
cd test
cmake -B build2 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM:STRING=3.5
cmake --build build2 --config Debug
.\build2\Debug\unit_tests.exe
.\build2\Debug\microreader_tests.exe
```

## Project Structure

```
lib/microreader/          shared core (platform-agnostic C++20)
  content/                EPUB parsing, layout, MRB binary format
  display/                Canvas, DisplayQueue, Font interfaces
  screens/                UI screen implementations
platforms/desktop/        SDL2 emulator
platforms/esp32/          ESP-IDF + PlatformIO firmware
test/                     Google Test suite
tools/                    Python scripts and dev tools
  calibre-plugin/         Calibre device plugin (build.py → microreader.zip)
docs/                     GitHub Pages — browser-based file manager (Web Serial API)
resources/                Fonts, sleep images
```

## Serial Command Line

`tools/serial_cmd.py` talks to the device over USB serial using the same CMND protocol as the browser manager.

```powershell
python tools/serial_cmd.py --port COM5 --upload "book.epub"
python tools/serial_cmd.py --port COM5 --list
python tools/serial_cmd.py --port COM5
```

## Hyphenation

Uses the [Liang/TeX algorithm](https://tug.org/docs/liang/) with pattern files compiled into binary tries by [Typst hypher](https://github.com/typst/hypher). Supported languages: EN, DE, FR, ES, IT, NL, PT, PL, RU.

## Firmware Backup & Restore

```powershell
# Backup full flash
python -m esptool --chip esp32c3 --port COM5 read_flash 0x0 0x1000000 firmware_backup.bin

# Restore
python -m esptool --chip esp32c3 --port COM5 write_flash 0x0 firmware_backup.bin

# Switch boot partition
python tools/switch_partition.py app0 --port COM5 --flash
```

## License

GPL v2 — see [LICENSE](LICENSE).
