#include "../src/native_rich.hpp"
#include <richedit.h>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* label) { if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); } }
static std::string narrow(const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr); return out;
}
static void select(HWND h, LONG a, LONG b) { CHARRANGE r{a, b}; SendMessageW(h, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&r)); }
static bool roundtrip(HWND h, const std::string& source, const char* label) {
    auto loaded = md::native::load(h, source);
    if (!loaded.editable) { std::fprintf(stderr, "load rejected [%s]: %s\n", label, narrow(loaded.reason).c_str()); check(false, label); return false; }
    std::string output; std::wstring error;
    if (!md::native::save(h, output, error)) { std::fprintf(stderr, "save rejected [%s]: %s\n", label, narrow(error).c_str()); check(false, label); return false; }
    loaded = md::native::load(h, output);
    check(loaded.editable, label);
    std::string again;
    check(md::native::save(h, again, error) && again == output, label);
    return true;
}
int main() {
    auto dll = LoadLibraryW(L"Msftedit.dll"); check(dll != nullptr, "load Msftedit");
    HWND h = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_POPUP | ES_MULTILINE | ES_WANTRETURN, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(h != nullptr, "create real RichEdit"); if (!h) return 1;
    SendMessageW(h, EM_EXLIMITTEXT, 0, 1024 * 1024);
    roundtrip(h, "", "empty document");
    roundtrip(h, "Hello 世界 😀\n", "unicode emoji");
    roundtrip(h, "first\nsecond\n\nthird", "soft break and EOF");
    roundtrip(h, "first  \nsecond", "hard break");
    roundtrip(h, "# 标题\n\n## Subtitle\n\n### H3\n\n#### H4\n\n##### H5\n\n###### H6\n", "all headings");
    roundtrip(h, "a **bold** and *italic* and ~~gone~~ and `x < y`", "inline styles");
    roundtrip(h, "**bold *both* tail**", "nested emphasis");
    roundtrip(h, "**hello world**", "formatted whitespace");
    roundtrip(h, "pre*italic*post a**bold**z **a*b*c**", "intra-word formatting");
    roundtrip(h, "- one\n- two\n\n3. three\n4. four", "simple lists");
    roundtrip(h, "> quote\n> continued\n>\n> next", "quote paragraphs");
    roundtrip(h, "```\nconst x = `value`;\n你好😀\n```", "code block");
    roundtrip(h, "```\n```", "empty code block");
    roundtrip(h, "a &amp; b &copy; &#x1f600;", "entities");
    roundtrip(h, "- \n- two", "empty bullet item");
    roundtrip(h, "`中文😀`", "inline code survives font fallback");
    for (const auto& text : std::vector<std::string>{
        "[link](https://example.com)", "![image](x.png)", "<custom>hello</custom>",
        "|a|b|\n|-|-|\n|1|2|", "$x^2$", "- one\n  - nested", "> > nested", "- [x] task",
        "[label]: https://example.com\n\nordinary", "```cpp\nint x;\n```", "- first\n\n  another paragraph"
    }) { auto r = md::native::load(h, text); check(!r.editable && !r.reason.empty(), "unsupported syntax rejects explicitly"); }
    md::native::load(h, "plain text"); select(h, 0, 5); md::native::toggleInline(h, 1);
    std::string output; std::wstring error;
    check(md::native::save(h, output, error) && output.find("**plain**") != std::string::npos, "toggle bold saves");
    md::native::setBlock(h, 2);
    check(md::native::save(h, output, error) && output.rfind("## ", 0) == 0, "set heading preserves inline");
    auto before = output; CHARRANGE old{}, after{}; SendMessageW(h, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&old));
    md::native::restyle(h, true); SendMessageW(h, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&after));
    check(old.cpMin == after.cpMin && old.cpMax == after.cpMax, "restyle preserves selection");
    check(md::native::save(h, output, error) && output == before, "restyle preserves semantic content");
    md::native::load(h, "hello"); select(h, 5, 5);
    SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L" * [literal] <html> 😀"));
    check(md::native::save(h, output, error), "plain paste punctuation escapes");
    check(md::native::load(h, output).editable, "plain paste roundtrip");
    md::native::load(h, "abc"); select(h, 0, 3);
    CHARFORMAT2W c{}; c.cbSize = sizeof(c); c.dwMask = CFM_UNDERLINE; c.dwEffects = CFE_UNDERLINE;
    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&c)); output = "unchanged";
    check(!md::native::save(h, output, error) && output == "unchanged" && !error.empty(), "unsupported native formatting does not overwrite output");
    md::native::load(h, "abc"); select(h, 0, 3); md::native::setBlock(h, 10);
    check(md::native::save(h, output, error) && output.rfind("```", 0) == 0, "toolbar code block");
    md::native::load(h, "abc"); select(h, 3, 3); SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\r\n"));
    check(md::native::save(h, output, error), "native Enter EOF");
    md::native::load(h, "abc"); select(h, 0, 3); md::native::toggleInline(h, 4);
    check(md::native::save(h, output, error) && output.find('`') != std::string::npos, "toolbar inline code");
    md::native::load(h, "3. first\n4. second"); select(h, 13, 13);
    SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\rthird"));
    check(md::native::save(h, output, error) && output.find("5. third") != std::string::npos, "list Enter increments number");
    md::native::load(h, "first"); select(h, 5, 5);
    SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\rsecond"));
    check(md::native::save(h, output, error) && output == "first\n\nsecond\n", "paragraph Enter remains paragraph");
    md::native::load(h, "abc"); select(h, 3, 3);
    SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"DEF"));
    md::native::restyle(h, true); SendMessageW(h, EM_UNDO, 0, 0);
    check(md::native::save(h, output, error) && output == "abc\n", "theme change does not consume undo");
    md::native::load(h, "`代码`  \nnext"); md::native::save(h, before, error);
    md::native::restyle(h, true); select(h, 0, 2);
    c = {}; c.cbSize = sizeof(c); SendMessageW(h, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&c));
    check(c.crBackColor == RGB(41, 45, 54), "dark inline code background");
    check(md::native::save(h, output, error) && output == before, "theme preserves code and hard break markers");
    md::native::load(h, "```\none\n```\n"); select(h, 3, 3);
    check(md::native::insertCodeLineBreak(h), "code Enter handled");
    SendMessageW(h, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"two"));
    bool codeSaved = md::native::save(h, output, error);
    if (!codeSaved || output != "```\none\ntwo\n```\n") std::fprintf(stderr, "code output=%s, error=%s\n", output.c_str(), narrow(error).c_str());
    check(codeSaved && output == "```\none\ntwo\n```\n", "code Enter keeps single fence");
    md::native::load(h, "```\none\n```\n"); select(h, 3, 3);
    check(md::native::insertCodeLineBreak(h), "code Enter before undo");
    SendMessageW(h, EM_UNDO, 0, 0);
    check(md::native::save(h, output, error) && output == "```\none\n```\n", "code Enter undoes in one action");
    md::native::load(h, "ordinary"); select(h, 8, 8);
    check(!md::native::insertCodeLineBreak(h), "ordinary Enter is not intercepted");
    DestroyWindow(h); FreeLibrary(dll);
    std::printf("native rich tests: %d failure(s)\n", failures); return failures ? 1 : 0;
}
