#define _WIN32_WINNT 0x0600
#define UNICODE
#define _UNICODE
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

#define WM_TRAYICON (WM_APP + 1)
#define WM_TRANSLATE_DONE (WM_APP + 200)
#define ID_TRAY_OPEN  2001
#define ID_TRAY_KEY   2002
#define ID_TRAY_EXIT  2003

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
HWND g_hProgress = NULL;
BOOL g_translating = FALSE;
WCHAR g_lastSlavicLang[64] = L"Russian";
HWND g_hPrevForegroundWindow = NULL;
NOTIFYICONDATAW g_nid;
HHOOK g_hKeyboardHook = NULL;
DWORD g_lastCtrlC = 0;
BOOL g_ctrlDown = FALSE;
WCHAR g_apiKey[256] = L"";
WCHAR g_configPath[MAX_PATH];
WCHAR g_settingsPath[MAX_PATH];
WCHAR g_exeDir[MAX_PATH];

// ---------- Persisted settings (loaded before the window exists, applied after) ----------
WCHAR g_savedSourceLang[64] = L"Auto-detect";
WCHAR g_savedTargetLang[64] = L"English";
BOOL g_savedAlwaysOnTop = FALSE;
BOOL g_savedAutoPaste = FALSE;
int g_savedWinX = CW_USEDEFAULT, g_savedWinY = CW_USEDEFAULT, g_savedWinW = 600, g_savedWinH = 520;

// ---------- Forward declarations ----------
void GetLangText(WCHAR* buf, int bufSize);
void GetSourceLangText(WCHAR* buf, int bufSize);

// ---------- Utility: config file (plain text next to exe) ----------
void LoadApiKey() {
    FILE* f = _wfopen(g_configPath, L"r, ccs=UTF-8");
    if (f) {
        fgetws(g_apiKey, 256, f);
        // strip newline
        size_t len = wcslen(g_apiKey);
        while (len > 0 && (g_apiKey[len-1] == L'\n' || g_apiKey[len-1] == L'\r')) {
            g_apiKey[--len] = 0;
        }
        fclose(f);
    }
}

void SaveApiKey(const WCHAR* key) {
    WCHAR trimmed[256];
    wcsncpy(trimmed, key, 255);
    trimmed[255] = 0;
    // Обрезаем пробелы и переносы строк по краям
    size_t start = 0;
    while (trimmed[start] == L' ' || trimmed[start] == L'\t' || trimmed[start] == L'\r' || trimmed[start] == L'\n') start++;
    size_t end = wcslen(trimmed);
    while (end > start && (trimmed[end-1] == L' ' || trimmed[end-1] == L'\t' || trimmed[end-1] == L'\r' || trimmed[end-1] == L'\n')) end--;
    trimmed[end] = 0;
    WCHAR* cleanKey = trimmed + start;

    FILE* f = _wfopen(g_configPath, L"w, ccs=UTF-8");
    if (f) {
        fputws(cleanKey, f);
        fclose(f);
    }
    wcsncpy(g_apiKey, cleanKey, 255);
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
        fwprintf(f, L"LastSlavicLang=%ls\n", g_lastSlavicLang);
        fwprintf(f, L"WinX=%d\n", (int)rc.left);
        fwprintf(f, L"WinY=%d\n", (int)rc.top);
        fwprintf(f, L"WinW=%d\n", (int)(rc.right - rc.left));
        fwprintf(f, L"WinH=%d\n", (int)(rc.bottom - rc.top));
        fclose(f);
    }
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

// ---------- Get selected language from combos ----------
void GetLangText(WCHAR* buf, int bufSize) {
    int idx = (int)SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0);
    SendMessageW(g_hCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
}

void GetSourceLangText(WCHAR* buf, int bufSize) {
    int idx = (int)SendMessageW(g_hSourceCombo, CB_GETCURSEL, 0, 0);
    SendMessageW(g_hSourceCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
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
} TranslateParams;

typedef struct {
    WCHAR* rawResult;   // malloc'd, or NULL on error
    WCHAR errBuf[4096];
} TranslateResultMsg;

DWORD WINAPI TranslateThreadProc(LPVOID param) {
    TranslateParams* p = (TranslateParams*)param;
    TranslateResultMsg* res = (TranslateResultMsg*)malloc(sizeof(TranslateResultMsg));
    res->errBuf[0] = 0;
    res->rawResult = CallGeminiTranslate(p->text, p->sourceLang, p->targetLang, res->errBuf, 4096);
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

// ---------- Simulate Ctrl+V in whatever window previously had focus ----------
void AutoPasteToPreviousWindow() {
    BOOL autoPaste = (SendMessageW(g_hAutoPasteCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!autoPaste) return;
    if (!g_hPrevForegroundWindow || !IsWindow(g_hPrevForegroundWindow)) return;

    HWND target = g_hPrevForegroundWindow;
    g_hPrevForegroundWindow = NULL;

    SetForegroundWindow(target);
    Sleep(60); // give the target window a moment to actually receive focus

    INPUT inputs[4];
    ZeroMemory(inputs, sizeof(inputs));
    inputs[0].type = INPUT_KEYBOARD; inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD; inputs[1].ki.wVk = 'V';
    inputs[2].type = INPUT_KEYBOARD; inputs[2].ki.wVk = 'V'; inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD; inputs[3].ki.wVk = VK_CONTROL; inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));

    ShowWindow(g_hMain, SW_HIDE);
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

LRESULT CALLBACK ApiKeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK) {
                WCHAR buf[256];
                GetWindowTextW(g_apiEditCtrl, buf, 256);
                SaveApiKey(buf);
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

void ShowApiKeyDialog(HWND parent) {
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

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ApiKeyDlgClass", L"Gemini API Key",
        (WS_POPUP | WS_CAPTION | WS_SYSMENU) & ~WS_MAXIMIZEBOX,
        300, 300, 440, 160, parent, NULL, GetModuleHandle(NULL), NULL);

    CreateWindowExW(0, L"STATIC", L"Enter API key (aistudio.google.com -> Get API key):",
        WS_CHILD | WS_VISIBLE, 10, 10, 400, 20, hDlg, NULL, GetModuleHandle(NULL), NULL);

    g_apiEditCtrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_apiKey,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        10, 40, 400, 24, hDlg, (HMENU)501, GetModuleHandle(NULL), NULL);

    HWND hOk = CreateWindowExW(0, L"BUTTON", L"Save",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        230, 82, 90, 28, hDlg, (HMENU)IDOK, GetModuleHandle(NULL), NULL);
    HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE,
        330, 82, 80, 28, hDlg, (HMENU)IDCANCEL, GetModuleHandle(NULL), NULL);

    HFONT hFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

// ---------- Reposition/resize controls to fill the current client area ----------
void LayoutControls(HWND hwnd) {
    if (!g_hInput || !g_hOutput) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    int margin = 12;
    int contentW = W - margin * 2;
    if (contentW < 200) contentW = 200;

    int inputTop = 96;
    int btn1W = 120, btn2W = 150, btnH = 34, btnGap = 8;
    int progressH = 6, progressGap1 = 6, progressGap2 = 8;
    int labelH = 22, labelGap = 4;
    int bottomMargin = margin;

    int fixedVert = inputTop + btnGap + btnH + progressGap1 + progressH + progressGap2 + labelH + labelGap + bottomMargin;
    int editsTotal = H - fixedVert;
    if (editsTotal < 120) editsTotal = 120;
    int inputH = editsTotal / 2;
    int outputH = editsTotal - inputH;
    if (inputH < 60) inputH = 60;
    if (outputH < 60) outputH = 60;

    MoveWindow(g_hSourceLabel, margin, 70, contentW, labelH, TRUE);
    MoveWindow(g_hInput, margin, inputTop, contentW, inputH, TRUE);

    int btnY = inputTop + inputH + btnGap;
    MoveWindow(g_hBtn, margin, btnY, btn1W, btnH, TRUE);
    MoveWindow(g_hBtnReverse, margin + btn1W + 8, btnY, btn2W, btnH, TRUE);
    int statusX = margin + btn1W + 8 + btn2W + 13;
    int statusW = contentW - (btn1W + 8 + btn2W + 13);
    if (statusW < 60) statusW = 60;
    MoveWindow(g_hStatus, statusX, btnY + (btnH - labelH) / 2, statusW, labelH, TRUE);

    int progressY = btnY + btnH + progressGap1;
    MoveWindow(g_hProgress, margin, progressY, contentW, progressH, TRUE);

    int outLabelY = progressY + progressH + progressGap2;
    MoveWindow(g_hOutputLabel, margin, outLabelY, contentW, labelH, TRUE);
    int outY = outLabelY + labelH + labelGap;
    MoveWindow(g_hOutput, margin, outY, contentW, outputH, TRUE);
}

// ---------- Main window proc ----------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HWND hSrcLabel = CreateWindowExW(0, L"STATIC", L"Source:", WS_CHILD | WS_VISIBLE,
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
                12, 44, 150, 22, hwnd, (HMENU)ID_CHECK_TOPMOST, GetModuleHandle(NULL), NULL);

            g_hAutoPasteCheck = CreateWindowExW(0, L"BUTTON", L"Auto paste",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                180, 44, 150, 22, hwnd, (HMENU)ID_CHECK_AUTOPASTE, GetModuleHandle(NULL), NULL);

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

            HFONT hFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HWND children[] = {hSrcLabel, g_hSourceCombo, g_hComboLabel, g_hCombo, g_hTopmostCheck, g_hAutoPasteCheck,
                                g_hSourceLabel, g_hInput, g_hBtn, g_hBtnReverse, g_hStatus, g_hOutputLabel, g_hOutput};
            for (int i = 0; i < 13; i++) SendMessageW(children[i], WM_SETFONT, (WPARAM)hFont, TRUE);

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

            LayoutControls(hwnd);

            // Restore persisted UI state
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
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = 540;
            mmi->ptMinTrackSize.y = 440;
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
            } else if (ctrlId == ID_TRAY_OPEN) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            } else if (ctrlId == ID_TRAY_KEY) {
                ShowApiKeyDialog(hwnd);
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
                AppendMenuW(hMenu, MF_STRING, ID_TRAY_KEY, L"Set API key");
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
    swprintf(g_settingsPath, MAX_PATH, L"%lspastetranslate_settings.ini", g_exeDir);
    LoadApiKey();
    LoadSettings();

    // Sanity-clamp restored window geometry in case of a corrupted file or a changed display setup
    if (g_savedWinW < 540 || g_savedWinW > 3000) g_savedWinW = 600;
    if (g_savedWinH < 440 || g_savedWinH > 3000) g_savedWinH = 520;
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

    if (wcslen(g_apiKey) == 0) {
        ShowWindow(g_hMain, SW_SHOW);
        ShowApiKeyDialog(g_hMain);
    }

    g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);

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
