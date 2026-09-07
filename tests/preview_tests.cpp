#include "preview.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <richedit.h>
#include <algorithm>
#include <cstring>
#endif

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
size_t count(std::string_view haystack, std::string_view needle) {
    size_t total = 0, offset = 0;
    while ((offset = haystack.find(needle, offset)) != std::string_view::npos) { ++total; offset += needle.size(); }
    return total;
}
bool balanced(std::string_view rtf) {
    int depth = 0;
    for (size_t i = 0; i < rtf.size(); ++i) {
        if (rtf[i] == '\\' && i + 1 < rtf.size() && (rtf[i + 1] == '{' || rtf[i + 1] == '}' || rtf[i + 1] == '\\')) { ++i; continue; }
        if (rtf[i] == '{') ++depth;
        if (rtf[i] == '}' && --depth < 0) return false;
        if (static_cast<unsigned char>(rtf[i]) > 127) return false;
    }
    return depth == 0;
}
std::string render(std::string_view input, md::PreviewOptions options = {}) {
    auto result = md::renderMarkdown(input, options);
    check(result.ok, "parser succeeds");
    check(balanced(result.rtf), "ASCII RTF groups balance");
    return result.rtf;
}
#ifdef _WIN32
struct StreamData { const std::string* bytes; size_t position = 0; };
DWORD CALLBACK readRtf(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG* read) {
    auto& data = *reinterpret_cast<StreamData*>(cookie);
    *read = static_cast<LONG>(std::min(static_cast<size_t>(size), data.bytes->size() - data.position));
    std::memcpy(buffer, data.bytes->data() + data.position, static_cast<size_t>(*read));
    data.position += *read;
    return 0;
}
void nativeRoundtrip() {
    HMODULE library = LoadLibraryW(L"Msftedit.dll");
    check(library != nullptr, "Windows RichEdit is available");
    if (!library) return;
    HWND window = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_POPUP | ES_MULTILINE, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "hidden RichEdit test window is created");
    if (window) {
        const auto rtf = render("***bolditalic*** plain 中文 😀\n\n| a | b |\n|---|---|\n|one|two|\n\nAfter table\n\n- parent\n  - child\n\n```\n{\\object fake}\n```\n");
        StreamData data{&rtf};
        EDITSTREAM stream{};
        stream.dwCookie = reinterpret_cast<DWORD_PTR>(&data);
        stream.pfnCallback = readRtf;
        SendMessageW(window, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream));
        check(stream.dwError == 0, "native RichEdit accepts generated RTF");
        std::wstring text(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
        GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        check(text.find(L"中文 \U0001f600") != std::wstring::npos, "native roundtrip preserves Chinese and emoji");
        check(text.find(L"one") != std::wstring::npos && text.find(L"two") != std::wstring::npos && text.find(L"After table") != std::wstring::npos, "native roundtrip preserves table and following paragraph");
        check(text.find(L"{\\object fake}") != std::wstring::npos, "native roundtrip leaves injection payload as visible text");
        CHARRANGE selection{0, 10};
        SendMessageW(window, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        SendMessageW(window, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
        check((format.dwEffects & (CFE_BOLD | CFE_ITALIC)) == (CFE_BOLD | CFE_ITALIC), "native RichEdit applies combined bold italic");
        const size_t plain = text.find(L"plain");
        selection = {static_cast<LONG>(plain), static_cast<LONG>(plain + 5)};
        SendMessageW(window, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        SendMessageW(window, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
        check((format.dwEffects & (CFE_BOLD | CFE_ITALIC)) == 0, "native RichEdit restores plain formatting after nested spans");
        const auto inspectTypography = [&](const md::PreviewOptions& options, int size, int spacing, int after) {
            const auto document = render("Body 中文\n\n# Heading\n\n`code`\n\n| header |\n|---|\n| cell |", options);
            StreamData input{&document};
            EDITSTREAM import{};
            import.dwCookie = reinterpret_cast<DWORD_PTR>(&input);
            import.pfnCallback = readRtf;
            SendMessageW(window, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&import));
            check(import.dwError == 0, "native RichEdit accepts typography options");
            CHARRANGE body{0, 4};
            SendMessageW(window, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&body));
            CHARFORMAT2W characters{};
            characters.cbSize = sizeof(characters);
            SendMessageW(window, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&characters));
            check(characters.yHeight == size * 10, "requested body size reaches native RichEdit in twips");
            PARAFORMAT2 paragraph{};
            paragraph.cbSize = sizeof(paragraph);
            SendMessageW(window, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));
            check(paragraph.bLineSpacingRule == 5 && paragraph.dyLineSpacing == spacing,
                "requested multiple line spacing reaches native RichEdit");
            check(paragraph.dySpaceAfter == after, "requested paragraph spacing reaches native RichEdit");
            if (options.fontFamily == "Calibri")
                check(std::wstring(characters.szFaceName) == L"Calibri", "selected font family reaches native RichEdit");
            if (options.fontFamily == "微软雅黑")
                check(std::wstring(characters.szFaceName) == L"微软雅黑", "Chinese font family reaches native RichEdit without mojibake");
            std::wstring visible(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
            GetWindowTextW(window, visible.data(), static_cast<int>(visible.size()));
            check(visible.find(L"Body 中文") == 0 && visible.find(L"header") != std::wstring::npos,
                "font metadata cannot leak into body or discard table content");
            const auto heading = visible.find(L"Heading");
            body = {static_cast<LONG>(heading), static_cast<LONG>(heading + 7)};
            SendMessageW(window, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&body));
            SendMessageW(window, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&characters));
            check(characters.yHeight > size * 10 && (characters.dwEffects & CFE_BOLD),
                "heading hierarchy follows selected body size");
            SendMessageW(window, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));
            check(paragraph.dySpaceBefore == after + 120 + (options.reading ? 80 : 0),
                "reading mode adds bounded heading breathing room");
        };
        inspectTypography({}, 26, 33, 180);
        md::PreviewOptions custom;
        custom.fontFamily = "Calibri";
        custom.fontSizeHalfPoints = 32;
        custom.lineHeightPercent = 180;
        custom.paragraphSpacingTwips = 240;
        custom.reading = true;
        inspectTypography(custom, 32, 36, 240);
        custom.fontFamily = "微软雅黑";
        inspectTypography(custom, 32, 36, 240);
        custom.fontFamily = "中文;{\\field malicious}\nFont";
        inspectTypography(custom, 32, 36, 240);
        custom.fontSizeHalfPoints = custom.lineHeightPercent = custom.paragraphSpacingTwips = std::numeric_limits<int>::min();
        inspectTypography(custom, 18, 24, 60);
        custom.fontSizeHalfPoints = custom.lineHeightPercent = custom.paragraphSpacingTwips = std::numeric_limits<int>::max();
        inspectTypography(custom, 56, 44, 480);
        DestroyWindow(window);
    }
    FreeLibrary(library);
}
#endif
}

int main() {
    auto rtf = render("```\n{\\object\\objdata 010203} \\field \\u123?\n```\n");
    check(rtf.find("\\{\\\\object\\\\objdata 010203\\}") != std::string::npos, "code cannot inject RTF object controls");
    check(rtf.find("\\\\field \\\\u123?") != std::string::npos, "backslashes stay text");
    rtf = render("中文 😀 &amp; &#x1F600; &NotEqualTilde; &#0; &#xD800; &#9999999; &unknown;\n");
    check(rtf.find("\\u20013?\\u25991?") != std::string::npos, "Chinese is encoded as Unicode");
    check(count(rtf, "\\u-10179?\\u-8704?") == 2, "emoji and numeric entity both use UTF-16 surrogate pair");
    check(rtf.find("\\u8770?\\u824?") != std::string::npos, "two-codepoint named entity is decoded");
    check(count(rtf, "\\u-3?") == 3, "invalid numeric Unicode becomes replacement characters");
    check(rtf.find("&unknown;") != std::string::npos, "unknown entity stays readable");
    rtf = render(std::string("bad ") + char(0xc0) + char(0xaf) + " " + char(0xed) + char(0xa0) + char(0x80));
    check(count(rtf, "\\u-3?") == 5, "overlong and surrogate UTF-8 cannot become control bytes");
    rtf = render("***bold italic*** and **outer ~~strike~~ outer** and `a{b}`.\n");
    check(rtf.find("{\\b {\\i bold italic}}") != std::string::npos || rtf.find("{\\i {\\b bold italic}}") != std::string::npos, "combined emphasis nests scoped groups");
    check(rtf.find("{\\b outer {\\strike strike} outer}") != std::string::npos, "nested strike restores outer bold");
    check(rtf.find("a\\{b\\}") != std::string::npos, "inline code escapes braces");
    rtf = render("3. parent\n   - child\n     - grandchild\n   - second\n4. sibling\n\n- [x] done\n- [ ] pending\n");
    check(rtf.find("3.\\tab parent") != std::string::npos && rtf.find("4.\\tab sibling") != std::string::npos, "ordered list preserves start and numbering");
    check(rtf.find("\\li1080") != std::string::npos, "nested list preserves indentation depth");
    check(count(rtf, "\\u8226?") == 3, "nested bullet count is correct");
    check(count(rtf, "\\u9745?") == 1 && count(rtf, "\\u9744?") == 1, "tasks render checked and unchecked markers");
    rtf = render("- first\n\n  continuation\n\n  - nested\n\n- second\n");
    check(count(rtf, "\\u8226?") == 3, "loose list continuation does not duplicate bullet");
    rtf = render("-\n  - child\n");
    check(count(rtf, "\\u8226?") == 2 && rtf.find("\\li360") < rtf.find("\\li720"), "empty parent marker precedes nested list");
    rtf = render("| Left | Center | Right |\n| :--- | :---: | ---: |\n| a | **b** | c |\n", {false, 6600});
    check(count(rtf, "\\trowd") == 2 && count(rtf, "\\cell}") == 6 && count(rtf, "\\row}") == 2, "table has two native rows and six cells");
    check(count(rtf, "\\cellx6600") == 2, "table fits requested width");
    check(rtf.find("\\qc") != std::string::npos && rtf.find("\\qr") != std::string::npos, "table alignments survive");
    std::string wide = "|";
    for (int i = 0; i < 20; ++i) wide += " h" + std::to_string(i) + " |";
    wide += "\n|";
    for (int i = 0; i < 20; ++i) wide += " --- |";
    wide += "\n|";
    for (int i = 0; i < 20; ++i) wide += " v" + std::to_string(i) + " |";
    wide += '\n';
    rtf = render(wide, {true, 4400});
    check(count(rtf, "\\trowd") == 10 && count(rtf, "\\cell}") == 40, "wide table wraps rows without dropping cells");
    check(rtf.find("v19") != std::string::npos, "final wide table value retained");
    rtf = render("[safe](javascript:alert) ![cat](https://example.com/cat.png)\n\n<script>alert('x')</script>\n");
    check(rtf.find("javascript:") == std::string::npos && rtf.find("https:") == std::string::npos, "link and image destinations never enter RTF");
    check(rtf.find("[Image: cat]") != std::string::npos, "image placeholder contains alt text");
    check(rtf.find("<script>alert('x')</script>") != std::string::npos, "raw HTML is visible text");
    check(rtf.find("\\field") == std::string::npos && rtf.find("\\object") == std::string::npos, "renderer emits no active fields or objects");
    render("# Heading\n\n> quote **bold**\n>\n> - nested\n\n---\n\n```cpp\na\nb\n```\n");
    render("");
    render("| a | b |\n|---|---|\n|c|d|", {false, std::numeric_limits<int>::min()});
    render("| a | b |\n|---|---|\n|c|d|", {false, std::numeric_limits<int>::max()});
    md::PreviewOptions options;
    check(options == md::PreviewOptions{}, "identical preview options compare equal");
    options.reading = true;
    check(options != md::PreviewOptions{}, "reading mode invalidates preview cache");
    options = {};
    options.fontFamily = "中文;{\\object\\objdata abc}\\field\nFont";
    rtf = render("Visible body", options);
    const auto fontTable = rtf.substr(rtf.find("{\\fonttbl"), rtf.find("{\\colortbl") - rtf.find("{\\fonttbl"));
    check(count(fontTable, ";") == 2, "font name cannot introduce a font-table terminator");
    check(fontTable.find("\\'e4\\'b8\\'ad\\'e6\\'96\\'87 \\'7b\\'5cobject") != std::string::npos,
        "Chinese font name and RTF syntax are escaped as data");
    check(fontTable.find("\\line") == std::string::npos, "font-name newlines do not emit paragraph controls");
    options.fontFamily.clear();
    check(render("body", options).find("Microsoft YaHei;") != std::string::npos, "empty font name uses readable Chinese default");
    // Repeated small documents catch state accidentally leaking between renders.
    for (int i = 0; i < 100; ++i) render(i % 2 ? "- x\n  - y" : "***hello***", {i % 2 != 0, 7500});
#ifdef _WIN32
    nativeRoundtrip();
#endif
    if (failures) { std::cerr << failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "All Markdown preview tests passed.\n";
    return EXIT_SUCCESS;
}
