# PasteTranslate

A tiny native Windows clipboard translator. Select text anywhere on your
PC, press **Ctrl+C twice**, and a small window pops up with the text
already translated — optionally pasted straight back where you copied it
from. Powered by Google's Gemini API.

Single self-contained `.exe`, no installer, no runtime, no dependencies
beyond standard Windows system DLLs.

## Features

- **Global hotkey — double Ctrl+C.** A low-level keyboard hook detects two
  Ctrl+C presses within 400 ms, reads the clipboard, and starts
  translating automatically. A normal single Ctrl+C still works exactly
  as before.
- **Source / Target language pickers** — `Auto-detect`, English,
  Ukrainian, Russian, Spanish.
- **Auto language detection**, done in the same API call as the
  translation itself (no extra round trip).
- **Script-mismatch auto-correction** — if you explicitly pick a Source
  language that doesn't match the actual script of the pasted text (e.g.
  Source = English but the text is Cyrillic), Source and Target are
  swapped automatically.
- **Smart target for Auto-detect** — pasting Latin-script text (English/
  Spanish) automatically targets whichever Slavic language (Russian or
  Ukrainian) you used most recently, instead of a stale Target selection.
- **Reverse-translate button (⇄)** — swaps Source/Target and re-translates
  the result back.
- **Always on top** toggle, re-asserted every time the hotkey fires.
- **Auto paste** toggle — remembers which window had focus before the
  hotkey stole it, and after translating, switches back and synthesizes
  `Ctrl+V` there, so the translation lands right where your cursor was.
- **Auto-copy to clipboard** on every successful translation.
- **Non-blocking UI** — translation runs on a background thread with an
  animated busy indicator; the window never freezes.
- **Resizable, theme-aware UI** using current Windows visual styles
  (Common Controls v6 manifest).
- **Single instance** — relaunching just focuses the existing window.
- **Settings persistence** — languages, checkboxes, window size/position
  are remembered between runs.
- **Ctrl+Enter to translate** while typing in the input box (plain Enter
  inserts a normal line break).
- **Line-break / special-character-safe** — round-trips multi-line text
  correctly in both directions.
- **System tray integration** — closing the window just hides it; the
  hotkey keeps working in the background.

## Getting an API key

PasteTranslate uses the Gemini API, not the Claude API. Get a free key at
[aistudio.google.com](https://aistudio.google.com/apikey), then paste it
into the app on first launch (or later via the tray icon → **Set API
key**). The key is stored in plain text in `translator_config.txt` next
to the exe.

## Building from source

You need [MinGW-w64](https://www.mingw-w64.org/) (a GCC cross/native
toolchain that can target Windows) and Python 3 with
[Pillow](https://pypi.org/project/Pillow/) to (re)generate the icon.

### On Linux (cross-compiling)

```bash
sudo apt-get install -y mingw-w64
pip install Pillow

# 1. Generate icon.ico
python3 gen_icon.py

# 2. Compile the resources (icon + manifest)
x86_64-w64-mingw32-windres translator.rc -O coff -o translator_res.o

# 3. Compile and link the app
x86_64-w64-mingw32-gcc -municode -mwindows translator.c translator_res.o \
    -o PasteTranslate.exe -lwinhttp -lshell32 -lcomctl32
```

### On Windows (native)

Install [MSYS2](https://www.msys2.org/), then from an **MSYS2 MinGW64**
shell:

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-python-pillow

python gen_icon.py
windres translator.rc -O coff -o translator_res.o
gcc -municode -mwindows translator.c translator_res.o \
    -o PasteTranslate.exe -lwinhttp -lshell32 -lcomctl32
```

The result is a single ~150 KB `PasteTranslate.exe` — no other files are
required to run it (settings/config files are created next to it on
first use).

## Project layout

| File | Purpose |
|---|---|
| `translator.c` | The entire application (single-file C source) |
| `translator.rc` | Resource script — embeds the icon and the manifest |
| `app.manifest` | Enables Common Controls v6 (modern visual styles) + DPI awareness |
| `gen_icon.py` | Generates `icon.ico` (Pillow) |

## Notes

- The exe is unsigned, so Windows SmartScreen / some antivirus engines
  may flag it on first run — this is expected for an unsigned binary that
  installs a global keyboard hook, not a sign of malicious behavior.
- The Gemini API key is separate from any Anthropic/Claude subscription
  or API key — they are unrelated products.

## License

No license has been chosen yet for this project.
