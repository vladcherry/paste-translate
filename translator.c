#define _WIN32_WINNT 0x0600
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

#define ID_EDIT_INPUT   1001
#define ID_EDIT_OUTPUT  1002
#define ID_COMBO_LANG   1003
#define ID_BTN_TRANSLATE 1004
#define ID_STATIC_STATUS 1005
#define ID_CHECK_TOPMOST 1006
#define ID_COMBO_SOURCE_LANG 1007
#define ID_PROGRESS 1008
#define ID_BTN_REVERSE 1009
#define ID_CHECK_AUTOPASTE 1010
#define ID_COMBO_PROVIDER 1011
#define ID_BTN_PASTE 1012

#define WM_TRAYICON (WM_APP + 1)
#define WM_TRANSLATE_DONE (WM_APP + 200)
#define ID_TRAY_OPEN  2001
#define ID_TRAY_KEY   2002
#define ID_TRAY_EXIT  2003
#define ID_TRAY_KEY_GEMINI 2004

#define HOTKEY_TIMEOUT_MS 400
#define MUTEX_NAME L"PasteTranslate_SingleInstance_Mutex_9F3D2A1B"

HWND g_hMain = NULL;
HWND g_hInput, g_hOutput, g_hCombo, g_hBtn, g_hStatus;
HWND g_hBtnReverse = NULL;
HWND g_hSourceLabel = NULL;
HWND g_hOutputLabel = NULL;
HWND g_hComboLabel = NULL;
HWND g_hSourceCombo = NULL;
HWND g_hTopmostCheck = NULL;
HWND g_hAutoPasteCheck = NULL;
HWND g_hBtnPaste = NULL;
HWND g_hProgress = NULL;
BOOL g_translating = FALSE;
WCHAR g_lastSlavicLang[64] = L"Russian";
// Target for an automatic paste right after a hotkey-triggered translation.
// Deliberately cleared on manual Translate/Reverse so those never paste anywhere.
HWND g_hPrevForegroundWindow = NULL;
// Last window that held focus before ours, tracked continuously via WM_ACTIVATE.
// This is what the manual Paste button aims at, so it works even when the window
// was opened from the tray rather than by the hotkey.
HWND g_hLastForegroundWindow = NULL;
NOTIFYICONDATAW g_nid;
HHOOK g_hKeyboardHook = NULL;
HWINEVENTHOOK g_hForegroundHook = NULL;
DWORD g_lastCtrlC = 0;
BOOL g_ctrlDown = FALSE;
WCHAR g_apiKey[256] = L"";
WCHAR g_configPath[MAX_PATH];
WCHAR g_settingsPath[MAX_PATH];
WCHAR g_exeDir[MAX_PATH];

// ---------- DeepL provider state ----------
WCHAR g_deeplKey[256] = L"";
WCHAR g_deeplConfigPath[MAX_PATH];
HWND g_hProviderCombo = NULL;
HWND g_hProviderLabel = NULL;

// ---------- Layout metrics ----------
HFONT g_hFont = NULL;
HWND g_hSrcLangLabel = NULL;
int g_dpi = 96;
int g_minWinW = 560;
int g_minWinH = 470;

// ---------- Persisted settings (loaded before the window exists, applied after) ----------
WCHAR g_savedSourceLang[64] = L"Auto-detect";
WCHAR g_savedTargetLang[64] = L"English";
BOOL g_savedAlwaysOnTop = FALSE;
BOOL g_savedAutoPaste = FALSE;
WCHAR g_savedProvider[32] = L"DeepL";
int g_savedWinX = CW_USEDEFAULT, g_savedWinY = CW_USEDEFAULT, g_savedWinW = 600, g_savedWinH = 520;

// ---------- Forward declarations ----------
void GetLangText(WCHAR* buf, int bufSize);
void GetSourceLangText(WCHAR* buf, int bufSize);
void GetProviderText(WCHAR* buf, int bufSize);

// ---------- DPI + text measurement helpers ----------
// Every hardcoded offset below goes through Scale(), and every label width is
// measured with the actual font, so nothing can overlap at non-100% scaling.
static int Scale(int v) {
    return MulDiv(v, g_dpi, 96);
}

static void InitDpi(HWND hwnd) {
    g_dpi = 96;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        GetDpiForWindowFn getDpiForWindow =
            (GetDpiForWindowFn)(void*)GetProcAddress(user32, "GetDpiForWindow");
        if (getDpiForWindow) {
            UINT dpi = getDpiForWindow(hwnd);
            if (dpi >= 72 && dpi <= 480) {
                g_dpi = (int)dpi;
                return;
            }
        }
    }
    // Pre-Windows-10 fallback: system-wide DPI.
    HDC hdc = GetDC(NULL);
    if (hdc) {
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);
        if (dpi >= 72) g_dpi = dpi;
    }
}

static int MeasureTextWidth(const WCHAR* text) {
    HDC hdc = GetDC(g_hMain);
    if (!hdc) return 0;
    HFONT oldFont = g_hFont ? (HFONT)SelectObject(hdc, g_hFont) : NULL;
    SIZE size = {0, 0};
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &size);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(g_hMain, hdc);
    return (int)size.cx;
}

// Width of a control's own caption, as actually rendered.
static int MeasureCaptionWidth(HWND hwnd) {
    WCHAR buf[256] = L"";
    GetWindowTextW(hwnd, buf, 256);
    return MeasureTextWidth(buf);
}

// A CBS_DROPDOWNLIST combo ignores the height passed to MoveWindow for its closed
// state (that height sizes the drop-down list), so read back what it actually is.
static int ComboClosedHeight(HWND combo) {
    RECT rc;
    GetWindowRect(combo, &rc);
    return (int)(rc.bottom - rc.top);
}

// ---------- Utility: config file (plain text next to exe) ----------
static void LoadKeyFile(const WCHAR* path, WCHAR* dest, int destSize) {
    dest[0] = 0;
    FILE* f = _wfopen(path, L"r, ccs=UTF-8");
    if (f) {
        if (fgetws(dest, destSize, f)) {
            size_t len = wcslen(dest);
            while (len > 0 && (dest[len-1] == L'\n' || dest[len-1] == L'\r')) {
                dest[--len] = 0;
            }
        }
        fclose(f);
    }
}

static void SaveKeyFile(const WCHAR* path, const WCHAR* key, WCHAR* dest, int destSize) {
    WCHAR trimmed[256];
    wcsncpy(trimmed, key, 255);
    trimmed[255] = 0;
    // Trim surrounding whitespace and newlines
    size_t start = 0;
    while (trimmed[start] == L' ' || trimmed[start] == L'\t' || trimmed[start] == L'\r' || trimmed[start] == L'\n') start++;
    size_t end = wcslen(trimmed);
    while (end > start && (trimmed[end-1] == L' ' || trimmed[end-1] == L'\t' || trimmed[end-1] == L'\r' || trimmed[end-1] == L'\n')) end--;
    trimmed[end] = 0;
    WCHAR* cleanKey = trimmed + start;

    FILE* f = _wfopen(path, L"w, ccs=UTF-8");
    if (f) {
        fputws(cleanKey, f);
        fclose(f);
    }
    wcsncpy(dest, cleanKey, destSize - 1);
    dest[destSize - 1] = 0;
}

void LoadApiKeys() {
    LoadKeyFile(g_configPath, g_apiKey, 256);
    LoadKeyFile(g_deeplConfigPath, g_deeplKey, 256);
}

// ---------- Settings file (key=value lines, plain text next to exe) ----------
void LoadSettings() {
    FILE* f = _wfopen(g_settingsPath, L"r, ccs=UTF-8");
    if (!f) return;
    WCHAR line[512];
    while (fgetws(line, 512, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len-1] == L'\n' || line[len-1] == L'\r')) line[--len] = 0;
        WCHAR* eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        WCHAR* key = line;
        WCHAR* val = eq + 1;
        if (wcscmp(key, L"SourceLang") == 0) wcsncpy(g_savedSourceLang, val, 63);
        else if (wcscmp(key, L"TargetLang") == 0) wcsncpy(g_savedTargetLang, val, 63);
        else if (wcscmp(key, L"AlwaysOnTop") == 0) g_savedAlwaysOnTop = (wcscmp(val, L"1") == 0);
        else if (wcscmp(key, L"AutoPaste") == 0) g_savedAutoPaste = (wcscmp(val, L"1") == 0);
        else if (wcscmp(key, L"Provider") == 0) wcsncpy(g_savedProvider, val, 31);
        else if (wcscmp(key, L"LastSlavicLang") == 0) wcsncpy(g_lastSlavicLang, val, 63);
        else if (wcscmp(key, L"WinX") == 0) g_savedWinX = _wtoi(val);
        else if (wcscmp(key, L"WinY") == 0) g_savedWinY = _wtoi(val);
        else if (wcscmp(key, L"WinW") == 0) g_savedWinW = _wtoi(val);
        else if (wcscmp(key, L"WinH") == 0) g_savedWinH = _wtoi(val);
    }
    fclose(f);
}

void SaveSettings(HWND hwnd) {
    WCHAR srcLang[64] = L"", tgtLang[64] = L"";
    GetSourceLangText(srcLang, 64);
    GetLangText(tgtLang, 64);
    BOOL alwaysOnTop = (SendMessageW(g_hTopmostCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL autoPaste = (SendMessageW(g_hAutoPasteCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    RECT rc;
    GetWindowRect(hwnd, &rc);

    FILE* f = _wfopen(g_settingsPath, L"w, ccs=UTF-8");
    if (f) {
        fwprintf(f, L"SourceLang=%ls\n", srcLang);
        fwprintf(f, L"TargetLang=%ls\n", tgtLang);
        fwprintf(f, L"AlwaysOnTop=%d\n", alwaysOnTop ? 1 : 0);
        fwprintf(f, L"AutoPaste=%d\n", autoPaste ? 1 : 0);
        {
            WCHAR provider[32] = L"";
            GetProviderText(provider, 32);
            fwprintf(f, L"Provider=%ls\n", provider);
        }
        fwprintf(f, L"LastSlavicLang=%ls\n", g_lastSlavicLang);
        fwprintf(f, L"WinX=%d\n", (int)rc.left);
        fwprintf(f, L"WinY=%d\n", (int)rc.top);
        fwprintf(f, L"WinW=%d\n", (int)(rc.right - rc.left));
        fwprintf(f, L"WinH=%d\n", (int)(rc.bottom - rc.top));
        fclose(f);
    }
}

// ---------- Track the last foreground window that is not ours ----------
// WM_ACTIVATE cannot do this job: its lParam only carries the other window's
// handle when that window lives on the same thread, and is NULL for every
// cross-process switch. EVENT_SYSTEM_FOREGROUND reports the handle regardless.
// Shell surfaces can take the foreground (clicking the taskbar, showing the
// desktop) but have no caret, so pasting into them silently does nothing.
static BOOL IsShellWindow(HWND hwnd) {
    WCHAR cls[64] = L"";
    GetClassNameW(hwnd, cls, 64);
    return (wcscmp(cls, L"Shell_TrayWnd") == 0 ||
            wcscmp(cls, L"Progman") == 0 ||
            wcscmp(cls, L"WorkerW") == 0 ||
            wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
            wcscmp(cls, L"NotifyIconOverflowWindow") == 0 ||
            wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0);
}

static BOOL IsUsablePasteTarget(HWND hwnd) {
    if (!hwnd || hwnd == g_hMain) return FALSE;
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return FALSE;
    if (IsShellWindow(hwnd)) return FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return FALSE;   // our own windows, incl. the key dialog
    return TRUE;
}

void CALLBACK ForegroundEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild, DWORD eventThread, DWORD eventTime) {
    (void)hook; (void)event; (void)idChild; (void)eventThread; (void)eventTime;
    if (idObject != OBJID_WINDOW) return;
    if (!IsUsablePasteTarget(hwnd)) return;

    g_hLastForegroundWindow = hwnd;
}

// Fallback for the very first run, before any foreground switch was observed:
// the topmost visible, unowned, titled top-level window sitting behind ours.
static HWND FindWindowBehindUs(void) {
    for (HWND h = GetWindow(g_hMain, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsUsablePasteTarget(h)) continue;
        if (GetWindow(h, GW_OWNER)) continue;
        if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        if (GetWindowTextLengthW(h) == 0) continue;
        return h;
    }
    return NULL;
}

// ---------- Clipboard ----------
BOOL GetClipboardTextW(WCHAR* buf, int bufSize) {
    buf[0] = 0;
    if (!OpenClipboard(NULL)) return FALSE;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        WCHAR* pText = (WCHAR*)GlobalLock(hData);
        if (pText) {
            wcsncpy(buf, pText, bufSize - 1);
            buf[bufSize - 1] = 0;
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return buf[0] != 0;
}

void SetClipboardTextW(const WCHAR* text) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t len = wcslen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(WCHAR));
    if (hMem) {
        WCHAR* dst = (WCHAR*)GlobalLock(hMem);
        if (dst) {
            wcscpy(dst, text);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
}

// ---------- Normalize lone \n into \r\n so line breaks render correctly in Win32
// multiline EDIT controls (which require CRLF), and other special characters survive intact.
// Returns a malloc'd string; caller must free() it.
WCHAR* NormalizeToCRLF(const WCHAR* text) {
    size_t len = wcslen(text);
    WCHAR* out = (WCHAR*)malloc((len * 2 + 1) * sizeof(WCHAR));
    WCHAR* o = out;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == L'\n' && (i == 0 || text[i-1] != L'\r')) {
            *o++ = L'\r';
            *o++ = L'\n';
        } else {
            *o++ = text[i];
        }
    }
    *o = 0;
    return out;
}

// ---------- UTF-8 <-> UTF-16 helpers ----------
char* WideToUtf8(const WCHAR* wstr) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    char* buf = (char*)malloc(size);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, buf, size, NULL, NULL);
    return buf;
}

WCHAR* Utf8ToWide(const char* str) {
    int size = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    WCHAR* buf = (WCHAR*)malloc(size * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, str, -1, buf, size);
    return buf;
}

// ---------- JSON string escaping (UTF-8 bytes -> escaped JSON string content) ----------
// Appends escaped content of utf8 string src into dst (dst must be large enough)
void JsonEscapeAppend(char* dst, const char* src) {
    char* d = dst + strlen(dst);
    for (const unsigned char* s = (const unsigned char*)src; *s; s++) {
        switch (*s) {
            case '"': *d++ = '\\'; *d++ = '"'; break;
            case '\\': *d++ = '\\'; *d++ = '\\'; break;
            case '\n': *d++ = '\\'; *d++ = 'n'; break;
            case '\r': *d++ = '\\'; *d++ = 'r'; break;
            case '\t': *d++ = '\\'; *d++ = 't'; break;
            default:
                if (*s < 0x20) {
                    // Any other control character: escape as \u00XX to keep the JSON valid
                    sprintf(d, "\\u%04x", *s);
                    d += 6;
                } else {
                    *d++ = *s;
                }
        }
    }
    *d = 0;
}

// ---------- Extract first "text":"..." field from JSON (handles \" \\ \n escapes) ----------
char* ExtractTextField(const char* json) {
    const char* key = "\"text\"";
    const char* p = strstr(json, key);
    if (!p) return NULL;
    const char* s = p + strlen(key);
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s != ':') return NULL;
    s++;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s != '"') return NULL;
    s++; // skip opening quote
    const char* tf = s;
    size_t cap = strlen(tf) + 1;
    char* out = (char*)malloc(cap);
    char* o = out;
    while (*s) {
        if (*s == '\\') {
            char n = *(s + 1);
            if (n == 'n') { *o++ = '\n'; s += 2; }
            else if (n == 'r') { *o++ = '\r'; s += 2; }
            else if (n == 't') { *o++ = '\t'; s += 2; }
            else if (n == '"') { *o++ = '"'; s += 2; }
            else if (n == '\\') { *o++ = '\\'; s += 2; }
            else if (n == 'u') {
                // \uXXXX - copy raw hex value as a codepoint (basic handling, assume BMP)
                char hex[5] = {s[2], s[3], s[4], s[5], 0};
                unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
                // encode cp as utf8 (BMP only)
                if (cp < 0x80) { *o++ = (char)cp; }
                else if (cp < 0x800) {
                    *o++ = (char)(0xC0 | (cp >> 6));
                    *o++ = (char)(0x80 | (cp & 0x3F));
                } else {
                    *o++ = (char)(0xE0 | (cp >> 12));
                    *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                    *o++ = (char)(0x80 | (cp & 0x3F));
                }
                s += 6;
            } else { *o++ = n; s += 2; }
            continue;
        }
        if (*s == '"') break;
        *o++ = *s++;
    }
    *o = 0;
    return out;
}

// ---------- DeepL language code mapping ----------
// DeepL target codes want an explicit regional variant for English.
static const char* DeepLTargetCode(const WCHAR* uiLang) {
    if (wcscmp(uiLang, L"English") == 0) return "EN-US";
    if (wcscmp(uiLang, L"Ukrainian") == 0) return "UK";
    if (wcscmp(uiLang, L"Russian") == 0) return "RU";
    if (wcscmp(uiLang, L"Spanish") == 0) return "ES";
    return NULL;
}

// Returns NULL for "Auto-detect" (the source_lang field is then omitted entirely).
static const char* DeepLSourceCode(const WCHAR* uiLang) {
    if (wcscmp(uiLang, L"English") == 0) return "EN";
    if (wcscmp(uiLang, L"Ukrainian") == 0) return "UK";
    if (wcscmp(uiLang, L"Russian") == 0) return "RU";
    if (wcscmp(uiLang, L"Spanish") == 0) return "ES";
    return NULL;
}

// Maps a DeepL language code from the response back to the UI language name.
static const WCHAR* DeepLCodeToName(const char* code) {
    if (_stricmp(code, "EN") == 0 || _strnicmp(code, "EN-", 3) == 0) return L"English";
    if (_stricmp(code, "UK") == 0) return L"Ukrainian";
    if (_stricmp(code, "RU") == 0) return L"Russian";
    if (_stricmp(code, "ES") == 0 || _strnicmp(code, "ES-", 3) == 0) return L"Spanish";
    return NULL;
}

// ---------- Extract a flat "key":"value" string field from JSON ----------
static BOOL ExtractStringField(const char* json, const char* key, char* out, int outSize) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return FALSE;
    const char* s = p + strlen(pattern);
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s != ':') return FALSE;
    s++;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s != '"') return FALSE;
    s++;
    int i = 0;
    while (*s && *s != '"' && i < outSize - 1) out[i++] = *s++;
    out[i] = 0;
    return TRUE;
}

// ---------- One DeepL HTTP request against a specific host ----------
// Returns TRUE if a response was received at all; fills *statusOut and a malloc'd *respOut.
static BOOL DeepLRequest(const WCHAR* host, const WCHAR* key, const char* body,
                         DWORD* statusOut, char** respOut) {
    *statusOut = 0;
    *respOut = NULL;
    BOOL ok = FALSE;

    HINTERNET hSession = WinHttpOpen(L"PasteTranslate/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/v2/translate",
                NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (hRequest) {
                WCHAR headers[512];
                // %ls, not %s: this toolchain's msvcrt swprintf treats %s as char*.
                swprintf(headers, 512, L"Content-Type: application/json\r\nAuthorization: DeepL-Auth-Key %ls\r\n", key);

                BOOL sent = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
                    (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0);
                if (sent && WinHttpReceiveResponse(hRequest, NULL)) {
                    DWORD statusCode = 0, statusSize = sizeof(statusCode);
                    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

                    size_t cap = 8192, len = 0;
                    char* resp = (char*)malloc(cap);
                    resp[0] = 0;
                    DWORD dwSize = 0;
                    do {
                        dwSize = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                        if (dwSize == 0) break;
                        if (len + dwSize + 1 > cap) {
                            cap = (len + dwSize + 1) * 2;
                            resp = (char*)realloc(resp, cap);
                        }
                        DWORD dwRead = 0;
                        if (!WinHttpReadData(hRequest, resp + len, dwSize, &dwRead)) break;
                        len += dwRead;
                        resp[len] = 0;
                    } while (dwSize > 0);

                    *statusOut = statusCode;
                    *respOut = resp;
                    ok = TRUE;
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    }
    return ok;
}

// ---------- Call DeepL API ----------
// Returns a malloc'd wide string shaped exactly like the Gemini path's output
// ("LANG: ...\nTRANSLATION: ..."), so every UI layer above stays provider-agnostic.
WCHAR* CallDeepLTranslate(const WCHAR* text, const WCHAR* sourceLang, const WCHAR* targetLang, WCHAR* errOut, int errOutSize) {
    errOut[0] = 0;
    if (wcslen(g_deeplKey) == 0) {
        wcsncpy(errOut, L"DeepL API key not set (tray menu -> Set DeepL API key)", errOutSize - 1);
        return NULL;
    }

    const char* tgtCode = DeepLTargetCode(targetLang);
    if (!tgtCode) {
        swprintf(errOut, errOutSize, L"DeepL: unsupported target language '%ls'", targetLang);
        return NULL;
    }
    const char* srcCode = DeepLSourceCode(sourceLang);

    char* textUtf8 = WideToUtf8(text);
    size_t bodyCap = strlen(textUtf8) * 7 + 512;
    char* body = (char*)malloc(bodyCap);
    body[0] = 0;
    strcat(body, "{\"text\":[\"");
    JsonEscapeAppend(body, textUtf8);
    strcat(body, "\"],\"target_lang\":\"");
    strcat(body, tgtCode);
    strcat(body, "\"");
    if (srcCode) {
        strcat(body, ",\"source_lang\":\"");
        strcat(body, srcCode);
        strcat(body, "\"");
    }
    // preserve_formatting keeps DeepL from "fixing" leading/trailing punctuation and case.
    strcat(body, ",\"preserve_formatting\":true}");
    free(textUtf8);

    // Keys issued on the old free tier carry a ":fx" suffix and belong to the api-free
    // host; everything else belongs to api.deepl.com. DeepL has renamed its plans more
    // than once, so treat this only as a first guess and retry the other host on 403.
    size_t keyLen = wcslen(g_deeplKey);
    BOOL isFreeKey = (keyLen > 3 && wcscmp(g_deeplKey + keyLen - 3, L":fx") == 0);
    const WCHAR* primaryHost  = isFreeKey ? L"api-free.deepl.com" : L"api.deepl.com";
    const WCHAR* fallbackHost = isFreeKey ? L"api.deepl.com" : L"api-free.deepl.com";

    DWORD status = 0;
    char* resp = NULL;
    BOOL got = DeepLRequest(primaryHost, g_deeplKey, body, &status, &resp);
    if (got && status == 403) {
        DWORD altStatus = 0;
        char* altResp = NULL;
        if (DeepLRequest(fallbackHost, g_deeplKey, body, &altStatus, &altResp) && altStatus != 403) {
            free(resp);
            status = altStatus;
            resp = altResp;
        } else {
            free(altResp);
        }
    }
    free(body);

    if (!got) {
        wcsncpy(errOut, L"Failed to send request (check your internet connection)", errOutSize - 1);
        free(resp);
        return NULL;
    }

    WCHAR* result = NULL;
    if (status == 200) {
        // Response: {"translations":[{"detected_source_language":"EN","text":"..."}]}
        char* translatedUtf8 = ExtractTextField(resp);
        if (translatedUtf8) {
            char detected[16] = "";
            const WCHAR* langName = NULL;
            if (ExtractStringField(resp, "detected_source_language", detected, 16)) {
                langName = DeepLCodeToName(detected);
            }
            WCHAR* transW = Utf8ToWide(translatedUtf8);
            free(translatedUtf8);

            WCHAR langW[64];
            if (langName) {
                wcsncpy(langW, langName, 63);
            } else if (detected[0]) {
                WCHAR* rawCode = Utf8ToWide(detected);
                wcsncpy(langW, rawCode, 63);
                free(rawCode);
            } else {
                wcsncpy(langW, sourceLang, 63);
            }
            langW[63] = 0;

            size_t outLen = wcslen(transW) + 128;
            result = (WCHAR*)malloc(outLen * sizeof(WCHAR));
            swprintf(result, outLen, L"LANG: %ls\nTRANSLATION: %ls", langW, transW);
            free(transW);
        } else {
            wcsncpy(errOut, L"Failed to parse DeepL response", errOutSize - 1);
        }
    } else {
        WCHAR* respW = Utf8ToWide(resp);
        const WCHAR* hint = L"";
        if (status == 403) hint = L"\r\n\r\nAuthorization failed. Both DeepL hosts were tried, so the key itself is wrong, expired, or not an API key.";
        else if (status == 456) hint = L"\r\n\r\nCharacter quota for this plan is exhausted.";
        else if (status == 429) hint = L"\r\n\r\nToo many requests - wait a moment and retry.";
        else if (status == 413) hint = L"\r\n\r\nText too large for a single request.";
        swprintf(errOut, errOutSize, L"DeepL Error (%lu): %ls%ls", status, respW, hint);
        free(respW);
    }
    free(resp);
    return result;
}

// ---------- Call Gemini API ----------
// Returns malloc'd wide string with translation, or NULL on failure. errOut gets error message if any.
WCHAR* CallGeminiTranslate(const WCHAR* text, const WCHAR* sourceLang, const WCHAR* targetLang, WCHAR* errOut, int errOutSize) {
    errOut[0] = 0;
    if (wcslen(g_apiKey) == 0) {
        wcsncpy(errOut, L"API key not set", errOutSize - 1);
        return NULL;
    }

    char* textUtf8 = WideToUtf8(text);
    char* targetUtf8 = WideToUtf8(targetLang);
    char* sourceUtf8 = WideToUtf8(sourceLang);

    size_t bodyCap = strlen(textUtf8) * 7 + strlen(targetUtf8) * 2 + strlen(sourceUtf8) * 2 + 1024;
    char* body = (char*)malloc(bodyCap);
    body[0] = 0;
    strcat(body, "{\"contents\":[{\"parts\":[{\"text\":\"");
    {
        char promptPrefix[768];
        BOOL isAuto = (wcscmp(sourceLang, L"Auto-detect") == 0);
        if (isAuto) {
            snprintf(promptPrefix, sizeof(promptPrefix),
                "Detect the source language of the following text and translate it to %s. "
                "Respond in EXACTLY this format and nothing else:\\nLANG: <detected source language, in English>\\nTRANSLATION: <the translation>\\n\\nText:\\n", targetUtf8);
        } else {
            snprintf(promptPrefix, sizeof(promptPrefix),
                "Translate the following text from %s to %s. "
                "Respond in EXACTLY this format and nothing else:\\nLANG: %s\\nTRANSLATION: <the translation>\\n\\nText:\\n", sourceUtf8, targetUtf8, sourceUtf8);
        }
        JsonEscapeAppend(body, promptPrefix);
    }
    JsonEscapeAppend(body, textUtf8);
    strcat(body, "\"}]}]}");

    free(textUtf8);
    free(targetUtf8);
    free(sourceUtf8);

    WCHAR* result = NULL;
    HINTERNET hSession = WinHttpOpen(L"TranslatorApp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        HINTERNET hConnect = WinHttpConnect(hSession, L"generativelanguage.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/v1beta/models/gemini-flash-latest:generateContent",
                NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (hRequest) {
                WCHAR headers[512];
                swprintf(headers, 512, L"Content-Type: application/json\r\nx-goog-api-key: %ls\r\n", g_apiKey);

                BOOL sent = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
                    (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0);
                if (sent && WinHttpReceiveResponse(hRequest, NULL)) {
                    DWORD statusCode = 0, statusSize = sizeof(statusCode);
                    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

                    // Read full response
                    size_t cap = 8192, len = 0;
                    char* resp = (char*)malloc(cap);
                    DWORD dwSize = 0;
                    do {
                        dwSize = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                        if (dwSize == 0) break;
                        if (len + dwSize + 1 > cap) {
                            cap = (len + dwSize + 1) * 2;
                            resp = (char*)realloc(resp, cap);
                        }
                        DWORD dwRead = 0;
                        if (!WinHttpReadData(hRequest, resp + len, dwSize, &dwRead)) break;
                        len += dwRead;
                        resp[len] = 0;
                    } while (dwSize > 0);

                    if (statusCode == 200) {
                        char* translatedUtf8 = ExtractTextField(resp);
                        if (translatedUtf8) {
                            result = Utf8ToWide(translatedUtf8);
                            free(translatedUtf8);
                        } else {
                            wcsncpy(errOut, L"Failed to parse API response", errOutSize - 1);
                        }
                    } else {
                        WCHAR* respW = Utf8ToWide(resp);
                        swprintf(errOut, errOutSize, L"API Error (%lu): %ls", statusCode, respW);
                        free(respW);
                    }
                    free(resp);
                } else {
                    wcsncpy(errOut, L"Failed to send request (check your internet connection)", errOutSize - 1);
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    }
    free(body);
    return result;
}

// ---------- Provider dispatch ----------
WCHAR* CallTranslate(const WCHAR* provider, const WCHAR* text, const WCHAR* sourceLang,
                     const WCHAR* targetLang, WCHAR* errOut, int errOutSize) {
    if (wcscmp(provider, L"Gemini") == 0) {
        return CallGeminiTranslate(text, sourceLang, targetLang, errOut, errOutSize);
    }
    return CallDeepLTranslate(text, sourceLang, targetLang, errOut, errOutSize);
}

// ---------- Get selected language from combos ----------
void GetLangText(WCHAR* buf, int bufSize) {
    int idx = (int)SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0);
    SendMessageW(g_hCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
}

void GetSourceLangText(WCHAR* buf, int bufSize) {
    int idx = (int)SendMessageW(g_hSourceCombo, CB_GETCURSEL, 0, 0);
    SendMessageW(g_hSourceCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
}

// Falls back to the persisted value when the combo does not exist yet.
void GetProviderText(WCHAR* buf, int bufSize) {
    buf[0] = 0;
    int idx = g_hProviderCombo ? (int)SendMessageW(g_hProviderCombo, CB_GETCURSEL, 0, 0) : CB_ERR;
    if (idx == CB_ERR) {
        wcsncpy(buf, g_savedProvider, bufSize - 1);
        buf[bufSize - 1] = 0;
        return;
    }
    SendMessageW(g_hProviderCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
}

// ---------- Select a combo item by exact/prefix text match (no-op if not found) ----------
BOOL SetComboSelectionByText(HWND combo, const WCHAR* text) {
    int idx = (int)SendMessageW(combo, CB_FINDSTRINGEXACT, -1, (LPARAM)text);
    if (idx == CB_ERR) {
        idx = (int)SendMessageW(combo, CB_FINDSTRING, -1, (LPARAM)text);
    }
    if (idx != CB_ERR) {
        SendMessageW(combo, CB_SETCURSEL, idx, 0);
        return TRUE;
    }
    return FALSE;
}

// ---------- Heuristic: does the text contain Cyrillic characters? ----------
BOOL ContainsCyrillic(const WCHAR* text) {
    for (const WCHAR* p = text; *p; p++) {
        if ((*p >= 0x0400 && *p <= 0x04FF) || (*p >= 0x0500 && *p <= 0x052F)) {
            return TRUE;
        }
    }
    return FALSE;
}

// ---------- Parse "LANG: ...\nTRANSLATION: ..." structured response ----------
// Returns TRUE and fills langBuf + mallocs *translationOut if the markers were found.
BOOL ParseLangAndTranslation(const WCHAR* raw, WCHAR* langBuf, int langBufSize, WCHAR** translationOut) {
    const WCHAR* langMarker = L"LANG:";
    const WCHAR* transMarker = L"TRANSLATION:";
    const WCHAR* lp = wcsstr(raw, langMarker);
    const WCHAR* tp = wcsstr(raw, transMarker);
    if (!lp || !tp || tp < lp) return FALSE;

    lp += wcslen(langMarker);
    while (*lp == L' ' || *lp == L'\t') lp++;
    const WCHAR* langEnd = wcschr(lp, L'\n');
    if (!langEnd || langEnd > tp) langEnd = tp;
    int llen = (int)(langEnd - lp);
    if (llen < 0) llen = 0;
    if (llen >= langBufSize) llen = langBufSize - 1;
    wcsncpy(langBuf, lp, llen);
    langBuf[llen] = 0;
    int e = llen;
    while (e > 0 && (langBuf[e-1] == L' ' || langBuf[e-1] == L'\r' || langBuf[e-1] == L'\n' || langBuf[e-1] == L'\t')) langBuf[--e] = 0;

    const WCHAR* ts = tp + wcslen(transMarker);
    while (*ts == L' ' || *ts == L'\t' || *ts == L'\r' || *ts == L'\n') ts++;
    *translationOut = _wcsdup(ts);
    return TRUE;
}

// ---------- Background translation thread ----------
typedef struct {
    WCHAR text[8192];
    WCHAR targetLang[64];
    WCHAR sourceLang[64];
    WCHAR provider[32];
} TranslateParams;

typedef struct {
    WCHAR* rawResult;   // malloc'd, or NULL on error
    WCHAR errBuf[4096];
} TranslateResultMsg;

DWORD WINAPI TranslateThreadProc(LPVOID param) {
    TranslateParams* p = (TranslateParams*)param;
    TranslateResultMsg* res = (TranslateResultMsg*)malloc(sizeof(TranslateResultMsg));
    res->errBuf[0] = 0;
    res->rawResult = CallTranslate(p->provider, p->text, p->sourceLang, p->targetLang, res->errBuf, 4096);
    PostMessageW(g_hMain, WM_TRANSLATE_DONE, 0, (LPARAM)res);
    free(p);
    return 0;
}

// ---------- Kick off translation using current UI state (non-blocking) ----------
void DoTranslate() {
    if (g_translating) return;

    WCHAR inputBuf[8192];
    GetWindowTextW(g_hInput, inputBuf, 8192);
    if (wcslen(inputBuf) == 0) {
        SetWindowTextW(g_hOutput, L"");
        return;
    }

    // Pre-flight check: if an explicit (non Auto-detect) Source language obviously doesn't
    // match the script of the actual text (e.g. Source=English but the text is Cyrillic),
    // swap Source and Target so the direction makes sense.
    WCHAR srcSel[64];
    GetSourceLangText(srcSel, 64);
    if (wcscmp(srcSel, L"Auto-detect") != 0) {
        BOOL hasCyrillic = ContainsCyrillic(inputBuf);
        BOOL srcIsCyrillicLang = (wcscmp(srcSel, L"Russian") == 0 || wcscmp(srcSel, L"Ukrainian") == 0);
        BOOL srcIsLatinLang = (wcscmp(srcSel, L"English") == 0 || wcscmp(srcSel, L"Spanish") == 0);
        BOOL mismatch = (srcIsCyrillicLang && !hasCyrillic) || (srcIsLatinLang && hasCyrillic);
        if (mismatch) {
            WCHAR curTarget[64];
            GetLangText(curTarget, 64);
            SetComboSelectionByText(g_hSourceCombo, curTarget);
            SetComboSelectionByText(g_hCombo, srcSel);
        }
    }

    TranslateParams* p = (TranslateParams*)malloc(sizeof(TranslateParams));
    wcsncpy(p->text, inputBuf, 8191);
    p->text[8191] = 0;
    GetLangText(p->targetLang, 64);
    GetSourceLangText(p->sourceLang, 64);
    GetProviderText(p->provider, 32);

    // If auto-detecting and the text looks Latin-script (English/Spanish, no Cyrillic),
    // prefer translating into whichever Slavic language (Russian/Ukrainian) was last used.
    if (wcscmp(p->sourceLang, L"Auto-detect") == 0 && !ContainsCyrillic(inputBuf)) {
        wcsncpy(p->targetLang, g_lastSlavicLang, 63);
        p->targetLang[63] = 0;
        SetComboSelectionByText(g_hCombo, p->targetLang);
    }

    g_translating = TRUE;
    EnableWindow(g_hBtn, FALSE);
    EnableWindow(g_hBtnReverse, FALSE);
    SetWindowTextW(g_hStatus, L"Translating...");
    SetWindowTextW(g_hOutput, L"");
    ShowWindow(g_hProgress, SW_SHOW);
    SendMessageW(g_hProgress, PBM_SETMARQUEE, TRUE, 30);

    HANDLE hThread = CreateThread(NULL, 0, TranslateThreadProc, p, 0, NULL);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        g_translating = FALSE;
        EnableWindow(g_hBtn, TRUE);
        EnableWindow(g_hBtnReverse, TRUE);
        ShowWindow(g_hProgress, SW_HIDE);
        SendMessageW(g_hProgress, PBM_SETMARQUEE, FALSE, 0);
        SetWindowTextW(g_hStatus, L"Failed to start translation");
        free(p);
    }
}

// ---------- Reverse translate: swap text + languages and re-translate ----------
void DoReverseTranslate() {
    if (g_translating) return;

    WCHAR outputText[8192];
    GetWindowTextW(g_hOutput, outputText, 8192);
    if (wcslen(outputText) == 0) return;

    WCHAR currentTarget[64];
    GetLangText(currentTarget, 64);
    WCHAR currentSource[64];
    GetSourceLangText(currentSource, 64);

    // The new target is whatever the (possibly auto-detected) source language was.
    // If source combo still says "Auto-detect", fall back to reading it from the label.
    WCHAR newTargetLang[64] = L"";
    if (wcscmp(currentSource, L"Auto-detect") == 0) {
        WCHAR labelText[256];
        GetWindowTextW(g_hSourceLabel, labelText, 256);
        WCHAR* markerPos = wcsstr(labelText, L"detected: ");
        if (markerPos) {
            markerPos += wcslen(L"detected: ");
            WCHAR* end = wcschr(markerPos, L')');
            int len = end ? (int)(end - markerPos) : (int)wcslen(markerPos);
            if (len >= 64) len = 63;
            wcsncpy(newTargetLang, markerPos, len);
            newTargetLang[len] = 0;
        }
    } else {
        wcscpy(newTargetLang, currentSource);
    }

    // Move the translated text into the input box
    SetWindowTextW(g_hInput, outputText);

    // Source becomes the previous target language
    SetComboSelectionByText(g_hSourceCombo, currentTarget);

    // Target becomes the previous (effective) source language, if we could resolve it
    if (wcslen(newTargetLang) > 0) {
        SetComboSelectionByText(g_hCombo, newTargetLang);
    }

    DoTranslate();
}

// ---------- Hide our window and synthesize Ctrl+V in the target window ----------
// Hiding first hands focus back before any keystroke is sent, so the paste always
// lands in the caret position the user was actually looking at.
// Windows only honours SetForegroundWindow from the process that owns the current
// foreground window or just handled input. Attaching to the target's input queue
// first makes the handover reliable even when that is borderline.
static void ForceForeground(HWND target) {
    DWORD targetThread = GetWindowThreadProcessId(target, NULL);
    DWORD thisThread = GetCurrentThreadId();
    BOOL attached = FALSE;

    if (targetThread && targetThread != thisThread) {
        attached = AttachThreadInput(thisThread, targetThread, TRUE);
    }
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    BringWindowToTop(target);
    SetForegroundWindow(target);
    SetFocus(target);
    if (attached) {
        AttachThreadInput(thisThread, targetThread, FALSE);
    }
}

static void PasteIntoWindow(HWND target) {
    if (!target || !IsWindow(target) || target == g_hMain) return;

    ShowWindow(g_hMain, SW_HIDE);
    ForceForeground(target);
    Sleep(80); // give the target window a moment to actually receive focus

    INPUT inputs[4];
    ZeroMemory(inputs, sizeof(inputs));
    inputs[0].type = INPUT_KEYBOARD; inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD; inputs[1].ki.wVk = 'V';
    inputs[2].type = INPUT_KEYBOARD; inputs[2].ki.wVk = 'V'; inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD; inputs[3].ki.wVk = VK_CONTROL; inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
}

// ---------- Automatic paste after a hotkey-triggered translation ----------
void AutoPasteToPreviousWindow() {
    BOOL autoPaste = (SendMessageW(g_hAutoPasteCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!autoPaste) return;
    if (!g_hPrevForegroundWindow || !IsWindow(g_hPrevForegroundWindow)) return;

    HWND target = g_hPrevForegroundWindow;
    g_hPrevForegroundWindow = NULL;
    PasteIntoWindow(target);
}

// ---------- Paste button: hide the window, drop the translation at the caret ----------
void DoManualPaste() {
    // Make sure the clipboard really holds the translation we are about to paste.
    // Successful translations are auto-copied already, but the clipboard may have
    // been overwritten since, and the user may have triggered this without one.
    WCHAR outputText[8192];
    GetWindowTextW(g_hOutput, outputText, 8192);
    if (wcslen(outputText) > 0) {
        SetClipboardTextW(outputText);
    }

    HWND target = g_hLastForegroundWindow;
    if (!IsUsablePasteTarget(target)) {
        target = FindWindowBehindUs();
    }
    if (!target) {
        SetWindowTextW(g_hStatus, L"No window to paste into");
        return;
    }

    // An explicit paste also consumes any pending automatic one.
    g_hPrevForegroundWindow = NULL;
    PasteIntoWindow(target);
}

// ---------- Apply the result of a finished background translation to the UI ----------
void ApplyTranslateResult(TranslateResultMsg* res) {
    g_translating = FALSE;
    EnableWindow(g_hBtn, TRUE);
    EnableWindow(g_hBtnReverse, TRUE);
    ShowWindow(g_hProgress, SW_HIDE);
    SendMessageW(g_hProgress, PBM_SETMARQUEE, FALSE, 0);

    if (res->rawResult) {
        WCHAR detectedLang[64] = L"";
        WCHAR* translationText = NULL;
        if (ParseLangAndTranslation(res->rawResult, detectedLang, 64, &translationText)) {
            WCHAR* displayText = NormalizeToCRLF(translationText);
            SetWindowTextW(g_hOutput, displayText);
            SetClipboardTextW(displayText);
            free(displayText);

            WCHAR curSrc[64];
            GetSourceLangText(curSrc, 64);
            if (wcscmp(curSrc, L"Auto-detect") == 0) {
                if (SetComboSelectionByText(g_hSourceCombo, detectedLang)) {
                    SetWindowTextW(g_hSourceLabel, L"Source text:");
                } else {
                    WCHAR labelBuf[160];
                    swprintf(labelBuf, 160, L"Source text (detected: %ls):", detectedLang);
                    SetWindowTextW(g_hSourceLabel, labelBuf);
                }
            } else {
                SetWindowTextW(g_hSourceLabel, L"Source text:");
            }

            free(translationText);
            SetWindowTextW(g_hStatus, L"Copied to clipboard");
        } else {
            WCHAR* displayText = NormalizeToCRLF(res->rawResult);
            SetWindowTextW(g_hOutput, displayText);
            SetClipboardTextW(displayText);
            free(displayText);
            SetWindowTextW(g_hSourceLabel, L"Source text:");
            SetWindowTextW(g_hStatus, L"Copied to clipboard");
        }
        free(res->rawResult);
        AutoPasteToPreviousWindow();
    } else {
        SetWindowTextW(g_hStatus, L"Error — see translation field");
        SetWindowTextW(g_hOutput, res->errBuf);
        g_hPrevForegroundWindow = NULL;
    }
    free(res);
}

// ---------- API key dialog: proper window class with real event handling ----------
BOOL g_apiDlgActive = FALSE;
HWND g_apiEditCtrl = NULL;
WCHAR g_apiDlgProvider[32] = L"DeepL";

LRESULT CALLBACK ApiKeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                WCHAR buf[256];
                GetWindowTextW(g_apiEditCtrl, buf, 256);
                if (wcscmp(g_apiDlgProvider, L"Gemini") == 0) {
                    SaveKeyFile(g_configPath, buf, g_apiKey, 256);
                } else {
                    SaveKeyFile(g_deeplConfigPath, buf, g_deeplKey, 256);
                }
                DestroyWindow(hwnd);
            } else if (id == IDCANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_apiDlgActive = FALSE;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowApiKeyDialog(HWND parent, const WCHAR* provider) {
    static BOOL classRegistered = FALSE;
    if (!classRegistered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = ApiKeyWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"ApiKeyDlgClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        classRegistered = TRUE;
    }

    BOOL isGemini = (wcscmp(provider, L"Gemini") == 0);
    wcsncpy(g_apiDlgProvider, provider, 31);
    g_apiDlgProvider[31] = 0;

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ApiKeyDlgClass",
        isGemini ? L"Gemini API Key" : L"DeepL API Key",
        (WS_POPUP | WS_CAPTION | WS_SYSMENU) & ~WS_MAXIMIZEBOX,
        Scale(300), Scale(300), Scale(480), Scale(180),
        parent, NULL, GetModuleHandle(NULL), NULL);

    // Lay the dialog out against its real client area so the buttons stay inside
    // the frame and flush with the right edge at any DPI.
    RECT dcr;
    GetClientRect(hDlg, &dcr);
    int dlgW = (int)dcr.right, dlgH = (int)dcr.bottom;
    int dm = Scale(12), dgap = Scale(8);
    int hintH = Scale(20), editH = Scale(26), dBtnH = Scale(28), dBtnW = Scale(92);

    CreateWindowExW(0, L"STATIC",
        isGemini ? L"Enter API key (aistudio.google.com -> Get API key):"
                 : L"Enter API key (deepl.com/pro-api -> Account -> API keys):",
        WS_CHILD | WS_VISIBLE, dm, dm, dlgW - dm * 2, hintH,
        hDlg, NULL, GetModuleHandle(NULL), NULL);

    g_apiEditCtrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", isGemini ? g_apiKey : g_deeplKey,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        dm, dm + hintH + Scale(4), dlgW - dm * 2, editH,
        hDlg, (HMENU)501, GetModuleHandle(NULL), NULL);

    int dBtnY = dlgH - dm - dBtnH;
    HWND hOk = CreateWindowExW(0, L"BUTTON", L"Save",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        dlgW - dm - dBtnW * 2 - dgap, dBtnY, dBtnW, dBtnH,
        hDlg, (HMENU)IDOK, GetModuleHandle(NULL), NULL);
    HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE,
        dlgW - dm - dBtnW, dBtnY, dBtnW, dBtnH,
        hDlg, (HMENU)IDCANCEL, GetModuleHandle(NULL), NULL);

    HFONT hFont = g_hFont ? g_hFont : CreateFontW(-Scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(g_apiEditCtrl, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

    ShowWindow(hDlg, SW_SHOW);
    SetFocus(g_apiEditCtrl);
    SendMessageW(g_apiEditCtrl, EM_SETSEL, 0, -1);

    EnableWindow(parent, FALSE);
    g_apiDlgActive = TRUE;

    MSG msg;
    while (g_apiDlgActive && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

// ---------- Reposition/resize every control for the current client area ----------
// Labels are sized from their measured text, controls are aligned on a shared
// baseline per row, and the Engine group is right-anchored so it can never run
// into the checkboxes.
void LayoutControls(HWND hwnd) {
    if (!g_hInput || !g_hOutput || !g_hProviderCombo || !g_hSrcLangLabel || !g_hBtnPaste) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;

    int margin   = Scale(12);
    int gap      = Scale(6);   // between a label and the control it describes
    int groupGap = Scale(16);  // between independent groups on the same row
    int rowGap   = Scale(10);  // between the two header rows
    int labelPad = Scale(6);   // slack so a label never clips its own text
    int contentW = W - margin * 2;
    if (contentW < Scale(220)) contentW = Scale(220);

    int rowH = ComboClosedHeight(g_hSourceCombo);
    if (rowH < Scale(22)) rowH = Scale(22);
    int labelH = rowH;                       // labels fill the row and centre their text
    int dropH  = Scale(200);                 // drop-down list extent for the combos

    // ---- Row 1:  Source: [combo]      Target: [combo] ----
    int row1Y = Scale(12);
    int srcLabelW = MeasureCaptionWidth(g_hSrcLangLabel) + labelPad;
    int tgtLabelW = MeasureCaptionWidth(g_hComboLabel) + labelPad;
    int comboSpace = contentW - srcLabelW - tgtLabelW - gap * 2 - groupGap;
    int comboW = comboSpace / 2;
    if (comboW > Scale(190)) comboW = Scale(190);
    if (comboW < Scale(92)) comboW = Scale(92);

    int x = margin;
    MoveWindow(g_hSrcLangLabel, x, row1Y, srcLabelW, labelH, TRUE);
    x += srcLabelW + gap;
    MoveWindow(g_hSourceCombo, x, row1Y, comboW, dropH, TRUE);
    x += comboW + groupGap;

    // Right-anchor the Target group so both header rows end on the same edge.
    int tgtComboX = margin + contentW - comboW;
    int tgtLabelX = tgtComboX - gap - tgtLabelW;
    if (tgtLabelX < x) {
        tgtLabelX = x;
        tgtComboX = tgtLabelX + tgtLabelW + gap;
    }
    MoveWindow(g_hComboLabel, tgtLabelX, row1Y, tgtLabelW, labelH, TRUE);
    MoveWindow(g_hCombo, tgtComboX, row1Y, comboW, dropH, TRUE);

    // ---- Row 2:  [x] Always on top   [x] Auto paste        Engine: [combo] ----
    int row2Y = row1Y + rowH + rowGap;
    int checkGlyphW = Scale(22);  // themed check box plus its gap before the text
    int cb1W = MeasureCaptionWidth(g_hTopmostCheck) + checkGlyphW;
    int cb2W = MeasureCaptionWidth(g_hAutoPasteCheck) + checkGlyphW;
    int engLabelW = MeasureCaptionWidth(g_hProviderLabel) + labelPad;
    int engComboW = Scale(124);

    x = margin;
    MoveWindow(g_hTopmostCheck, x, row2Y, cb1W, rowH, TRUE);
    x += cb1W + groupGap;
    MoveWindow(g_hAutoPasteCheck, x, row2Y, cb2W, rowH, TRUE);
    x += cb2W + groupGap;
    int pasteBtnW = MeasureCaptionWidth(g_hBtnPaste) + Scale(28);
    MoveWindow(g_hBtnPaste, x, row2Y, pasteBtnW, rowH, TRUE);
    x += pasteBtnW + groupGap;

    // Right-anchor the Engine group, then push it back only if the row is too narrow.
    int engComboX = margin + contentW - engComboW;
    int engLabelX = engComboX - gap - engLabelW;
    if (engLabelX < x) {
        engLabelX = x;
        engComboX = engLabelX + engLabelW + gap;
        int overflow = (engComboX + engComboW) - (margin + contentW);
        if (overflow > 0) {
            engComboW -= overflow;
            if (engComboW < Scale(84)) engComboW = Scale(84);
        }
    }
    MoveWindow(g_hProviderLabel, engLabelX, row2Y, engLabelW, labelH, TRUE);
    MoveWindow(g_hProviderCombo, engComboX, row2Y, engComboW, dropH, TRUE);

    // ---- Buttons: width follows the caption so nothing is clipped ----
    int btnH = Scale(30);
    int btnGap = Scale(8);
    int btn1W = MeasureCaptionWidth(g_hBtn) + Scale(36);
    int btn2W = MeasureCaptionWidth(g_hBtnReverse) + Scale(36);

    // ---- Vertical budget for the two edit boxes ----
    int textLabelH = Scale(20);
    int srcTextLabelY = row2Y + rowH + Scale(12);
    int inputTop = srcTextLabelY + textLabelH + Scale(3);
    int progressH = Scale(6), progressGap1 = Scale(6), progressGap2 = Scale(8);
    int labelGap = Scale(3);

    int fixedVert = inputTop + btnGap + btnH + progressGap1 + progressH
                  + progressGap2 + textLabelH + labelGap + margin;
    int editsTotal = H - fixedVert;
    int minEdit = Scale(56);
    if (editsTotal < minEdit * 2) editsTotal = minEdit * 2;
    int inputH = editsTotal / 2;
    int outputH = editsTotal - inputH;

    MoveWindow(g_hSourceLabel, margin, srcTextLabelY, contentW, textLabelH, TRUE);
    MoveWindow(g_hInput, margin, inputTop, contentW, inputH, TRUE);

    int btnY = inputTop + inputH + btnGap;
    MoveWindow(g_hBtn, margin, btnY, btn1W, btnH, TRUE);
    MoveWindow(g_hBtnReverse, margin + btn1W + btnGap, btnY, btn2W, btnH, TRUE);

    int statusX = margin + btn1W + btnGap + btn2W + Scale(12);
    int statusW = (margin + contentW) - statusX;
    if (statusW < Scale(40)) statusW = Scale(40);
    // Centre the status text against the taller buttons.
    MoveWindow(g_hStatus, statusX, btnY + (btnH - textLabelH) / 2, statusW, textLabelH, TRUE);

    int progressY = btnY + btnH + progressGap1;
    MoveWindow(g_hProgress, margin, progressY, contentW, progressH, TRUE);

    int outLabelY = progressY + progressH + progressGap2;
    MoveWindow(g_hOutputLabel, margin, outLabelY, contentW, textLabelH, TRUE);
    MoveWindow(g_hOutput, margin, outLabelY + textLabelH + labelGap, contentW, outputH, TRUE);
}

// ---------- Smallest window size that still fits every row without overlap ----------
static void ComputeMinWindowSize(HWND hwnd) {
    int margin = Scale(12), gap = Scale(6), groupGap = Scale(16), labelPad = Scale(6);
    int checkGlyphW = Scale(22);

    int row1 = MeasureCaptionWidth(g_hSrcLangLabel) + labelPad + gap + Scale(150)
             + groupGap
             + MeasureCaptionWidth(g_hComboLabel) + labelPad + gap + Scale(150);

    int row2 = MeasureCaptionWidth(g_hTopmostCheck) + checkGlyphW + groupGap
             + MeasureCaptionWidth(g_hAutoPasteCheck) + checkGlyphW + groupGap
             + MeasureCaptionWidth(g_hBtnPaste) + Scale(28) + groupGap
             + MeasureCaptionWidth(g_hProviderLabel) + labelPad + gap + Scale(124);

    int row3 = MeasureCaptionWidth(g_hBtn) + Scale(36) + Scale(8)
             + MeasureCaptionWidth(g_hBtnReverse) + Scale(36) + Scale(12) + Scale(130);

    int widest = row1;
    if (row2 > widest) widest = row2;
    if (row3 > widest) widest = row3;

    RECT rc = {0, 0, widest + margin * 2, Scale(420)};
    AdjustWindowRectEx(&rc, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    g_minWinW = (int)(rc.right - rc.left);
    g_minWinH = (int)(rc.bottom - rc.top);
}

// ---------- Main window proc ----------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            InitDpi(hwnd);

            // Creation coordinates below are placeholders: LayoutControls() positions
            // and sizes every control from measured text once the font is applied.
            g_hSrcLangLabel = CreateWindowExW(0, L"STATIC", L"Source:", WS_CHILD | WS_VISIBLE,
                12, 13, 60, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);

            g_hSourceCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                74, 10, 150, 200, hwnd, (HMENU)ID_COMBO_SOURCE_LANG, GetModuleHandle(NULL), NULL);
            SendMessageW(g_hSourceCombo, CB_ADDSTRING, 0, (LPARAM)L"Auto-detect");
            SendMessageW(g_hSourceCombo, CB_ADDSTRING, 0, (LPARAM)L"English");
            SendMessageW(g_hSourceCombo, CB_ADDSTRING, 0, (LPARAM)L"Ukrainian");
            SendMessageW(g_hSourceCombo, CB_ADDSTRING, 0, (LPARAM)L"Russian");
            SendMessageW(g_hSourceCombo, CB_ADDSTRING, 0, (LPARAM)L"Spanish");
            SendMessageW(g_hSourceCombo, CB_SETCURSEL, 0, 0);

            g_hComboLabel = CreateWindowExW(0, L"STATIC", L"Target:", WS_CHILD | WS_VISIBLE,
                240, 13, 60, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);

            g_hCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                302, 10, 150, 200, hwnd, (HMENU)ID_COMBO_LANG, GetModuleHandle(NULL), NULL);
            SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM)L"English");
            SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM)L"Ukrainian");
            SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM)L"Russian");
            SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM)L"Spanish");
            SendMessageW(g_hCombo, CB_SETCURSEL, 0, 0);

            g_hTopmostCheck = CreateWindowExW(0, L"BUTTON", L"Always on top",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                12, 44, 112, 22, hwnd, (HMENU)ID_CHECK_TOPMOST, GetModuleHandle(NULL), NULL);

            g_hAutoPasteCheck = CreateWindowExW(0, L"BUTTON", L"Auto paste",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                132, 44, 96, 22, hwnd, (HMENU)ID_CHECK_AUTOPASTE, GetModuleHandle(NULL), NULL);

            g_hBtnPaste = CreateWindowExW(0, L"BUTTON", L"Paste",
                WS_CHILD | WS_VISIBLE,
                240, 44, 80, 24, hwnd, (HMENU)ID_BTN_PASTE, GetModuleHandle(NULL), NULL);

            g_hProviderLabel = CreateWindowExW(0, L"STATIC", L"Engine:", WS_CHILD | WS_VISIBLE,
                240, 47, 54, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);

            g_hProviderCombo = CreateWindowExW(0, L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                296, 44, 130, 160, hwnd, (HMENU)ID_COMBO_PROVIDER, GetModuleHandle(NULL), NULL);
            SendMessageW(g_hProviderCombo, CB_ADDSTRING, 0, (LPARAM)L"DeepL");
            SendMessageW(g_hProviderCombo, CB_ADDSTRING, 0, (LPARAM)L"Gemini");
            SendMessageW(g_hProviderCombo, CB_SETCURSEL, 0, 0);

            g_hSourceLabel = CreateWindowExW(0, L"STATIC", L"Source text:", WS_CHILD | WS_VISIBLE,
                12, 70, 200, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);

            g_hInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                12, 96, 500, 130, hwnd, (HMENU)ID_EDIT_INPUT, GetModuleHandle(NULL), NULL);

            g_hBtn = CreateWindowExW(0, L"BUTTON", L"Translate",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                12, 234, 120, 34, hwnd, (HMENU)ID_BTN_TRANSLATE, GetModuleHandle(NULL), NULL);

            g_hBtnReverse = CreateWindowExW(0, L"BUTTON", L"\x21C4 Reverse",
                WS_CHILD | WS_VISIBLE,
                140, 234, 150, 34, hwnd, (HMENU)ID_BTN_REVERSE, GetModuleHandle(NULL), NULL);

            g_hStatus = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE,
                303, 243, 220, 22, hwnd, (HMENU)ID_STATIC_STATUS, GetModuleHandle(NULL), NULL);

            g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                WS_CHILD | PBS_MARQUEE,
                12, 276, 500, 6, hwnd, (HMENU)ID_PROGRESS, GetModuleHandle(NULL), NULL);

            g_hOutputLabel = CreateWindowExW(0, L"STATIC", L"Translation:", WS_CHILD | WS_VISIBLE,
                12, 290, 200, 22, hwnd, NULL, GetModuleHandle(NULL), NULL);

            g_hOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                12, 316, 500, 110, hwnd, (HMENU)ID_EDIT_OUTPUT, GetModuleHandle(NULL), NULL);

            g_hFont = CreateFontW(-Scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HWND children[] = {g_hSrcLangLabel, g_hSourceCombo, g_hComboLabel, g_hCombo,
                                g_hTopmostCheck, g_hAutoPasteCheck, g_hBtnPaste,
                                g_hProviderLabel, g_hProviderCombo,
                                g_hSourceLabel, g_hInput, g_hBtn, g_hBtnReverse, g_hStatus,
                                g_hOutputLabel, g_hOutput};
            for (int i = 0; i < (int)(sizeof(children) / sizeof(children[0])); i++) {
                SendMessageW(children[i], WM_SETFONT, (WPARAM)g_hFont, TRUE);
            }

            // Tray icon
            memset(&g_nid, 0, sizeof(g_nid));
            g_nid.cbSize = sizeof(NOTIFYICONDATAW);
            g_nid.hWnd = hwnd;
            g_nid.uID = 1;
            g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            g_nid.uCallbackMessage = WM_TRAYICON;
            g_nid.hIcon = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCEW(101));
            wcscpy(g_nid.szTip, L"PasteTranslate (Ctrl+C twice)");
            Shell_NotifyIconW(NIM_ADD, &g_nid);

            // Text metrics are known only now that the font is applied.
            ComputeMinWindowSize(hwnd);
            {
                RECT wr;
                GetWindowRect(hwnd, &wr);
                int w = (int)(wr.right - wr.left);
                int h = (int)(wr.bottom - wr.top);
                if (w < g_minWinW || h < g_minWinH) {
                    SetWindowPos(hwnd, NULL, 0, 0,
                                 w < g_minWinW ? g_minWinW : w,
                                 h < g_minWinH ? g_minWinH : h,
                                 SWP_NOMOVE | SWP_NOZORDER);
                }
            }
            LayoutControls(hwnd);

            // Restore persisted UI state
            SetComboSelectionByText(g_hProviderCombo, g_savedProvider);
            SetComboSelectionByText(g_hSourceCombo, g_savedSourceLang);
            SetComboSelectionByText(g_hCombo, g_savedTargetLang);
            if (g_savedAlwaysOnTop) {
                SendMessageW(g_hTopmostCheck, BM_SETCHECK, BST_CHECKED, 0);
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
            if (g_savedAutoPaste) {
                SendMessageW(g_hAutoPasteCheck, BM_SETCHECK, BST_CHECKED, 0);
            }
            break;
        }
        case WM_SIZE: {
            if (wParam != SIZE_MINIMIZED) {
                LayoutControls(hwnd);
            }
            break;
        }
        case WM_ACTIVATE: {
            // Secondary source only: lParam holds the deactivated window's handle
            // just for same-thread switches, and is NULL across processes, which is
            // why ForegroundEventProc above does the real tracking.
            if (LOWORD(wParam) != WA_INACTIVE) {
                HWND prev = (HWND)lParam;
                if (prev && prev != hwnd && IsWindow(prev)) {
                    g_hLastForegroundWindow = prev;
                }
            }
            break;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = g_minWinW;
            mmi->ptMinTrackSize.y = g_minWinH;
            break;
        }
        case WM_COMMAND: {
            int ctrlId = LOWORD(wParam);
            int notifyCode = HIWORD(wParam);
            if (ctrlId == ID_BTN_TRANSLATE) {
                g_hPrevForegroundWindow = NULL;
                DoTranslate();
            } else if (ctrlId == ID_BTN_REVERSE) {
                g_hPrevForegroundWindow = NULL;
                DoReverseTranslate();
            } else if (ctrlId == ID_BTN_PASTE) {
                DoManualPaste();
            } else if (ctrlId == ID_CHECK_TOPMOST) {
                BOOL checked = (SendMessageW(g_hTopmostCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetWindowPos(hwnd, checked ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            } else if (ctrlId == ID_COMBO_LANG && notifyCode == CBN_SELCHANGE) {
                WCHAR sel[64];
                GetLangText(sel, 64);
                if (wcscmp(sel, L"Russian") == 0 || wcscmp(sel, L"Ukrainian") == 0) wcscpy(g_lastSlavicLang, sel);
            } else if (ctrlId == ID_COMBO_SOURCE_LANG && notifyCode == CBN_SELCHANGE) {
                WCHAR sel[64];
                GetSourceLangText(sel, 64);
                if (wcscmp(sel, L"Russian") == 0 || wcscmp(sel, L"Ukrainian") == 0) wcscpy(g_lastSlavicLang, sel);
            } else if (ctrlId == ID_COMBO_PROVIDER && notifyCode == CBN_SELCHANGE) {
                // Switching to a provider with no key yet: ask for it right away.
                WCHAR provider[32];
                GetProviderText(provider, 32);
                if (wcscmp(provider, L"Gemini") == 0 && wcslen(g_apiKey) == 0) {
                    ShowApiKeyDialog(hwnd, L"Gemini");
                } else if (wcscmp(provider, L"DeepL") == 0 && wcslen(g_deeplKey) == 0) {
                    ShowApiKeyDialog(hwnd, L"DeepL");
                }
            } else if (ctrlId == ID_TRAY_OPEN) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            } else if (ctrlId == ID_TRAY_KEY) {
                ShowApiKeyDialog(hwnd, L"DeepL");
            } else if (ctrlId == ID_TRAY_KEY_GEMINI) {
                ShowApiKeyDialog(hwnd, L"Gemini");
            } else if (ctrlId == ID_TRAY_EXIT) {
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_TRAYICON: {
            if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            } else if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, L"Open PasteTranslate");
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_KEY, L"Set DeepL API key");
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_KEY_GEMINI, L"Set Gemini API key");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            break;
        }
        case WM_APP + 100: {
            // Custom message: hotkey triggered, wParam unused, populate from clipboard and translate
            WCHAR clip[8192];
            if (GetClipboardTextW(clip, 8192)) {
                g_hPrevForegroundWindow = GetForegroundWindow();
                WCHAR* normalizedClip = NormalizeToCRLF(clip);
                SetWindowTextW(g_hInput, normalizedClip);
                free(normalizedClip);
                ShowWindow(hwnd, SW_SHOW);
                if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                BOOL topmostChecked = (SendMessageW(g_hTopmostCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (topmostChecked) {
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                } else {
                    // Still bring to front above other (non-topmost) windows even without the option enabled
                    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                }
                DoTranslate();
            }
            break;
        }
        case WM_TRANSLATE_DONE: {
            TranslateResultMsg* res = (TranslateResultMsg*)lParam;
            ApplyTranslateResult(res);
            break;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            SaveSettings(hwnd);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_hKeyboardHook) UnhookWindowsHookEx(g_hKeyboardHook);
            if (g_hForegroundHook) UnhookWinEvent(g_hForegroundHook);
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------- Low-level keyboard hook to detect double Ctrl+C ----------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (kb->vkCode == VK_CONTROL || kb->vkCode == VK_LCONTROL || kb->vkCode == VK_RCONTROL) {
                g_ctrlDown = TRUE;
            } else if (kb->vkCode == 'C' && g_ctrlDown) {
                DWORD now = GetTickCount();
                if (now - g_lastCtrlC < HOTKEY_TIMEOUT_MS) {
                    g_lastCtrlC = 0;
                    PostMessageW(g_hMain, WM_APP + 100, 0, 0);
                } else {
                    g_lastCtrlC = now;
                }
            }
        } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            if (kb->vkCode == VK_CONTROL || kb->vkCode == VK_LCONTROL || kb->vkCode == VK_RCONTROL) {
                g_ctrlDown = FALSE;
            }
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(L"TranslatorAppWindowClass", NULL);
        if (hExisting) {
            ShowWindow(hExisting, SW_SHOW);
            if (IsIconic(hExisting)) ShowWindow(hExisting, SW_RESTORE);
            SetForegroundWindow(hExisting);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    WCHAR* lastSlash = wcsrchr(g_exeDir, L'\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    swprintf(g_configPath, MAX_PATH, L"%lstranslator_config.txt", g_exeDir);
    swprintf(g_deeplConfigPath, MAX_PATH, L"%lsdeepl_config.txt", g_exeDir);
    swprintf(g_settingsPath, MAX_PATH, L"%lspastetranslate_settings.ini", g_exeDir);
    LoadApiKeys();
    LoadSettings();

    // Sanity-clamp restored window geometry in case of a corrupted file or a changed
    // display setup. WM_CREATE grows the window afterwards if the real DPI needs more.
    if (g_savedWinW < 400 || g_savedWinW > 6000) g_savedWinW = 620;
    if (g_savedWinH < 360 || g_savedWinH > 6000) g_savedWinH = 540;
    if (g_savedWinX != CW_USEDEFAULT && (g_savedWinX < -100 || g_savedWinX > 10000)) g_savedWinX = CW_USEDEFAULT;
    if (g_savedWinY != CW_USEDEFAULT && (g_savedWinY < -100 || g_savedWinY > 10000)) g_savedWinY = CW_USEDEFAULT;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TranslatorAppWindowClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    RegisterClassW(&wc);

    g_hMain = CreateWindowExW(0, L"TranslatorAppWindowClass", L"PasteTranslate",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
        g_savedWinX, g_savedWinY, g_savedWinW, g_savedWinH,
        NULL, NULL, hInstance, NULL);

    // Only prompt for the key of the provider that is actually selected.
    {
        BOOL geminiSelected = (wcscmp(g_savedProvider, L"Gemini") == 0);
        const WCHAR* missingFor = NULL;
        if (geminiSelected && wcslen(g_apiKey) == 0) missingFor = L"Gemini";
        else if (!geminiSelected && wcslen(g_deeplKey) == 0) missingFor = L"DeepL";
        if (missingFor) {
            ShowWindow(g_hMain, SW_SHOW);
            ShowApiKeyDialog(g_hMain, missingFor);
        }
    }

    g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);

    // WINEVENT_OUTOFCONTEXT delivers through this thread's message loop, so no DLL
    // injection is needed; SKIPOWNPROCESS keeps our own windows out of the results.
    g_hForegroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, ForegroundEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            msg.hwnd == g_hInput && (GetKeyState(VK_CONTROL) & 0x8000)) {
            DoTranslate();
            continue; // swallow the keystroke so it doesn't also insert a newline
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
