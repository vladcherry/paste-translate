// Host-side test for the pure string/JSON logic of the DeepL path.
// Compiled natively on Linux with shims for the Win32-only bits, so the request
// body shape and the response parsing can be checked without Windows.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <strings.h>

#define BOOL int
#define TRUE 1
#define FALSE 0
#define WCHAR wchar_t
#define _stricmp strcasecmp
#define _strnicmp strncasecmp

// ---- copied verbatim from translator.c ----
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
                    sprintf(d, "\\u%04x", *s);
                    d += 6;
                } else {
                    *d++ = *s;
                }
        }
    }
    *d = 0;
}

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
    s++;
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
                char hex[5] = {s[2], s[3], s[4], s[5], 0};
                unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
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

static const char* DeepLTargetCode(const WCHAR* uiLang) {
    if (wcscmp(uiLang, L"English") == 0) return "EN-US";
    if (wcscmp(uiLang, L"Ukrainian") == 0) return "UK";
    if (wcscmp(uiLang, L"Russian") == 0) return "RU";
    if (wcscmp(uiLang, L"Spanish") == 0) return "ES";
    return NULL;
}

static const char* DeepLSourceCode(const WCHAR* uiLang) {
    if (wcscmp(uiLang, L"English") == 0) return "EN";
    if (wcscmp(uiLang, L"Ukrainian") == 0) return "UK";
    if (wcscmp(uiLang, L"Russian") == 0) return "RU";
    if (wcscmp(uiLang, L"Spanish") == 0) return "ES";
    return NULL;
}

static const WCHAR* DeepLCodeToName(const char* code) {
    if (_stricmp(code, "EN") == 0 || _strnicmp(code, "EN-", 3) == 0) return L"English";
    if (_stricmp(code, "UK") == 0) return L"Ukrainian";
    if (_stricmp(code, "RU") == 0) return L"Russian";
    if (_stricmp(code, "ES") == 0 || _strnicmp(code, "ES-", 3) == 0) return L"Spanish";
    return NULL;
}

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
// ---- end copied code ----

static int failures = 0;

static void check(const char* name, int ok, const char* got) {
    printf("%-46s %s%s%s\n", name, ok ? "PASS" : "FAIL",
           ok ? "" : "  got: ", ok ? "" : (got ? got : "(null)"));
    if (!ok) failures++;
}

// Mirrors the body assembly in CallDeepLTranslate.
static char* BuildBody(const char* textUtf8, const WCHAR* sourceLang, const WCHAR* targetLang) {
    const char* tgtCode = DeepLTargetCode(targetLang);
    if (!tgtCode) return NULL;
    const char* srcCode = DeepLSourceCode(sourceLang);
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
    strcat(body, ",\"preserve_formatting\":true}");
    return body;
}

int main(void) {
    // 1. Auto-detect source: source_lang must be omitted entirely.
    char* b1 = BuildBody("Hello world", L"Auto-detect", L"Russian");
    check("auto-detect body omits source_lang",
          strcmp(b1, "{\"text\":[\"Hello world\"],\"target_lang\":\"RU\",\"preserve_formatting\":true}") == 0, b1);

    // 2. Explicit source + English target maps to EN-US.
    char* b2 = BuildBody("Привет", L"Russian", L"English");
    check("explicit source, English -> EN-US",
          strcmp(b2, "{\"text\":[\"Привет\"],\"target_lang\":\"EN-US\",\"source_lang\":\"RU\",\"preserve_formatting\":true}") == 0, b2);

    // 3. Multiline text with quotes and a control char stays valid JSON.
    char* b3 = BuildBody("line1\nline2\t\"quoted\"\x01", L"Auto-detect", L"Ukrainian");
    check("newlines/quotes/control chars escaped",
          strstr(b3, "line1\\nline2\\t\\\"quoted\\\"\\u0001") != NULL, b3);

    // 4. Real DeepL response: first "text" field is the translation.
    const char* resp = "{\"translations\":[{\"detected_source_language\":\"EN\",\"text\":\"Привет, мир\"}]}";
    char* t = ExtractTextField(resp);
    check("response: translation extracted", t && strcmp(t, "Привет, мир") == 0, t);

    char det[16] = "";
    ExtractStringField(resp, "detected_source_language", det, 16);
    check("response: detected_source_language read", strcmp(det, "EN") == 0, det);
    check("detected code -> UI name",
          DeepLCodeToName(det) && wcscmp(DeepLCodeToName(det), L"English") == 0, det);

    // 5. Pretty-printed response variant (whitespace after the colon).
    const char* pretty = "{\n  \"translations\": [\n    {\n      \"detected_source_language\": \"UK\",\n"
                         "      \"text\": \"Hello\"\n    }\n  ]\n}";
    char* t2 = ExtractTextField(pretty);
    check("pretty-printed response parsed", t2 && strcmp(t2, "Hello") == 0, t2);
    char det2[16] = "";
    ExtractStringField(pretty, "detected_source_language", det2, 16);
    check("pretty-printed detected lang parsed", strcmp(det2, "UK") == 0, det2);
    check("UK -> Ukrainian",
          DeepLCodeToName(det2) && wcscmp(DeepLCodeToName(det2), L"Ukrainian") == 0, det2);

    // 6. Escaped newlines inside a DeepL translation survive.
    const char* multi = "{\"translations\":[{\"detected_source_language\":\"EN\",\"text\":\"Строка1\\nСтрока2\"}]}";
    char* t3 = ExtractTextField(multi);
    check("escaped \\n unescaped to real newline",
          t3 && strcmp(t3, "Строка1\nСтрока2") == 0, t3);

    // 7. Regional variants map back correctly.
    check("EN-GB -> English",
          DeepLCodeToName("EN-GB") && wcscmp(DeepLCodeToName("EN-GB"), L"English") == 0, "EN-GB");
    check("ES-419 -> Spanish",
          DeepLCodeToName("ES-419") && wcscmp(DeepLCodeToName("ES-419"), L"Spanish") == 0, "ES-419");
    check("unknown code -> NULL (falls back to raw)", DeepLCodeToName("DE") == NULL, "DE");

    // 8. Free-vs-Pro host selection by the ":fx" key suffix.
    const WCHAR* freeKey = L"279a2e9d-83b3-c416-7e2d-f721593e42a0:fx";
    const WCHAR* proKey  = L"279a2e9d-83b3-c416-7e2d-f721593e42a0";
    size_t fl = wcslen(freeKey), pl = wcslen(proKey);
    check("free key (:fx) -> api-free host",
          (fl > 3 && wcscmp(freeKey + fl - 3, L":fx") == 0), "free");
    check("pro key -> api.deepl.com host",
          !(pl > 3 && wcscmp(proKey + pl - 3, L":fx") == 0), "pro");

    printf("\n%s (%d failures)\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
