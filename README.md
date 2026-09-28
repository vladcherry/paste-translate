# PasteTranslate

A tiny native Windows clipboard translator. Select text anywhere on your
PC, press **Ctrl+C twice**, and a small window pops up with the text
already translated — optionally pasted straight back where you copied it
from. Two translation engines: **DeepL** (the default) or **Google's
Gemini** — pick one in the window, the choice is remembered.

Single self-contained `.exe`, no installer, no runtime, no dependencies
beyond standard Windows system DLLs.

Current release: **1.1.0**.

<p align="center">
  <a href="https://github.com/vladcherry/paste-translate/releases/latest/download/PasteTranslate.exe"><b>⬇ Download PasteTranslate.exe</b></a>
  &nbsp;·&nbsp;
  <a href="https://github.com/vladcherry/paste-translate/releases/latest">Latest release</a>
</p>

Download the exe, put it anywhere, and run it. On first launch it asks for
the API key of the selected engine — DeepL by default (see below).

## Features

- **Global hotkey — double Ctrl+C.** A low-level keyboard hook detects two
  Ctrl+C presses within 400 ms, reads the clipboard, and starts
  translating automatically. A normal single Ctrl+C still works exactly
  as before.
- **Engine picker — DeepL or Gemini.** A combo box in the second row of
  the window; switching takes effect on the next translation, with no
  restart. Each engine keeps its own API key, so both can be set up and
  you can switch between them at any time.
- **Source / Target language pickers** — `Auto-detect`, English,
  Ukrainian, Russian, Spanish.
- **Auto language detection**, with no extra round trip: Gemini reports
  the source language in the same call as the translation, DeepL returns
  it in `detected_source_language`.
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

## API keys — DeepL or Gemini

The app talks to one of two services, and each has its own key. Set up
whichever you prefer; setting up both lets you switch engines from the
window without touching anything else. On first launch the app only asks
for the key of the engine that is currently selected — DeepL unless you
change it — and switching to an engine with no key opens its dialog
straight away.

Both keys can also be entered at any time from the tray icon:
**Set DeepL API key** / **Set Gemini API key**.

### DeepL

1. Create an API account at
   [deepl.com/pro-api](https://www.deepl.com/pro-api) — the free tier
   covers 500 000 characters a month.
2. Open **Account → API keys** and copy the key.
3. Paste it into the app (first launch, or tray icon → **Set DeepL API
   key**).

The key itself decides which host is used: keys issued on the free tier
end in `:fx` and go to `api-free.deepl.com`, everything else goes to
`api.deepl.com`. If that first guess is rejected with a 403, the other
host is tried automatically, so a renamed plan does not break anything.
Once both hosts return 403 the error says so plainly — at that point the
key is wrong, expired, or not an API key (a DeepL account password or a
web-app login will not work here).

Requests go to `POST /v2/translate` with `preserve_formatting` on, so
leading punctuation, capitalisation and line breaks come back as they
were. `Auto-detect` simply omits `source_lang` and lets DeepL decide.
DeepL is used for the four languages the app offers — English (requested
as `EN-US`), Ukrainian, Russian and Spanish; a target outside that set is
reported as unsupported instead of failing silently.

### Gemini

This is the Gemini API, not the Claude API. Get a free key at
[aistudio.google.com](https://aistudio.google.com/apikey), then paste it
into the app (first launch, or tray icon → **Set Gemini API key**).
Translation and language detection happen in one call to
`gemini-flash-latest`.

### Where the keys and settings are stored

All three files are plain text, created next to the exe on first use:

| File | Contents |
|---|---|
| `deepl_config.txt` | The DeepL API key |
| `translator_config.txt` | The Gemini API key |
| `pastetranslate_settings.ini` | Languages, the selected engine (`Provider=`), checkboxes, window size and position |

## Building from source

### On Windows (MSVC) — what CI uses

Install [Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/)
with the **Desktop development with C++** workload, plus Python 3 with
[Pillow](https://pypi.org/project/Pillow/) for the icon, then run:

```bat
pip install Pillow
build.bat
```

`build.bat` finds MSVC through `vswhere` on its own (no Developer Command
Prompt needed), generates `icon.ico`, compiles the resources, and links
`build\PasteTranslate.exe`.

### With MinGW-w64

[MinGW-w64](https://www.mingw-w64.org/) works too, natively or
cross-compiling from Linux:

```bash
sudo apt-get install -y mingw-w64   # or: pacman -S mingw-w64-x86_64-gcc  (MSYS2)
pip install Pillow

python3 gen_icon.py
x86_64-w64-mingw32-windres translator.rc -O coff -o translator_res.o
x86_64-w64-mingw32-gcc -municode -mwindows translator.c translator_res.o \
    -o PasteTranslate.exe -lwinhttp -lshell32 -lcomctl32
```

On Windows under an **MSYS2 MinGW64** shell, drop the `x86_64-w64-mingw32-`
prefixes.

The result either way is a single ~210 KB `PasteTranslate.exe` — no other
files are required to run it (settings/config files are created next to it
on first use).

## Releases

Pushing a `v*` tag builds on `windows-latest` and publishes a release with
`PasteTranslate.exe` attached — see
[.github/workflows/build.yml](.github/workflows/build.yml):

```bash
git tag v1.1.0
git push origin v1.1.0
```

Every push to `main` and every pull request is built the same way, with the
exe kept as a workflow artifact.

## Project layout

| File | Purpose |
|---|---|
| `translator.c` | The entire application (single-file C source) |
| `translator.rc` | Resource script — embeds the icon and the manifest |
| `app.manifest` | Enables Common Controls v6 (modern visual styles) + DPI awareness |
| `gen_icon.py` | Generates `icon.ico` (Pillow) |
| `build.bat` | MSVC build script (used by CI and locally) |
| `test_deepl.c` | Host-side test of the DeepL request body and response parsing |
| `layout_check.py` | Mirrors the window layout arithmetic: checks that no control overlaps or escapes the client area at 96–192 dpi |

Neither test needs Windows:

```bash
gcc -o test_deepl test_deepl.c && ./test_deepl
python3 layout_check.py
```

## Notes

- The exe is unsigned, so Windows SmartScreen / some antivirus engines
  may flag it on first run — this is expected for an unsigned binary that
  installs a global keyboard hook, not a sign of malicious behavior.
- The Gemini API key is separate from any Anthropic/Claude subscription
  or API key — they are unrelated products. The same goes for DeepL: the
  app needs a DeepL **API** key, which is not the same thing as a
  DeepL Pro subscription for the website or the desktop app.
- Both keys are stored in plain text next to the exe. Anyone with access
  to that folder can read them.

## License

No license has been chosen yet for this project.
