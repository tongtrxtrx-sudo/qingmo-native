#include "native_rich.hpp"
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include "md4c.h"
extern "C" {
#include "entity.h"
}
#include <algorithm>
#include <array>
#include <cwchar>
#include <limits>
#include <stdexcept>
#include <vector>

namespace md::native {
namespace {
constexpr unsigned Bold = 1, Italic = 2, Strike = 4, Code = 8;
constexpr LONG QuoteIndent = 360, CodeIndent = 420;
constexpr COLORREF InlineLight = RGB(239, 242, 246), InlineDark = RGB(41, 45, 54), HardBreak = RGB(237, 242, 246);
struct Run { std::wstring text; unsigned flags = 0; };
struct Block { int style = 0; unsigned number = 1; std::vector<Run> runs; };
struct Model { std::vector<Block> blocks; };
std::wstring wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n) throw std::runtime_error("invalid UTF-8");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string utf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("invalid UTF-16");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
void add(Block& b, std::wstring text, unsigned flags) {
    if (text.empty()) return;
    if (!b.runs.empty() && b.runs.back().flags == flags) b.runs.back().text += text;
    else b.runs.push_back({std::move(text), flags});
}
void cp(std::wstring& s, unsigned value) {
    if (!value || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) value = 0xfffd;
    if (value <= 0xffff) s += static_cast<wchar_t>(value);
    else { value -= 0x10000; s += static_cast<wchar_t>(0xd800 + (value >> 10)); s += static_cast<wchar_t>(0xdc00 + (value & 1023)); }
}
std::wstring entity(std::string_view s) {
    std::wstring out;
    if (s.size() > 3 && s[1] == '#') {
        size_t i = 2; unsigned base = 10, value = 0;
        if (s[i] == 'x' || s[i] == 'X') { ++i; base = 16; }
        for (; i + 1 < s.size(); ++i) {
            unsigned d = s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : s[i] >= 'a' && s[i] <= 'f' ? s[i] - 'a' + 10 : s[i] >= 'A' && s[i] <= 'F' ? s[i] - 'A' + 10 : 100;
            if (d >= base || value > (0x10ffff - d) / base) { value = 0xfffd; break; }
            value = value * base + d;
        }
        cp(out, value);
    } else if (auto e = entity_lookup(s.data(), s.size())) {
        cp(out, e->codepoints[0]); if (e->codepoints[1]) cp(out, e->codepoints[1]);
    } else out = wide(s);
    return out;
}
struct Parser {
    Model model;
    std::wstring reason;
    int container = 0, active = -1, itemBlocks = 0;
    unsigned nextNumber = 1;
    bool inItem = false;
    std::array<unsigned, 4> spans{};
    int fail(const wchar_t* why) { reason = why; return 1; }
    unsigned flags() const { unsigned f = 0; for (size_t i = 0; i < 4; ++i) if (spans[i]) f |= 1u << i; return f; }
    int begin(MD_BLOCKTYPE type, void* detail) {
        switch (type) {
        case MD_BLOCK_DOC: return 0;
        case MD_BLOCK_QUOTE: case MD_BLOCK_UL: case MD_BLOCK_OL:
            if (container || active >= 0) return fail(L"嵌套列表或引用请在源码模式编辑。");
            container = type == MD_BLOCK_QUOTE ? 9 : type == MD_BLOCK_UL ? 7 : 8;
            if (type == MD_BLOCK_OL) {
                nextNumber = static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start;
                if (!nextNumber || nextNumber > 65535) return fail(L"此列表起始编号超出原生编辑范围。");
            }
            return 0;
        case MD_BLOCK_LI:
            if (inItem || (container != 7 && container != 8)) return fail(L"复杂列表请在源码模式编辑。");
            if (static_cast<MD_BLOCK_LI_DETAIL*>(detail)->is_task) return fail(L"任务列表请在源码模式编辑。");
            inItem = true; itemBlocks = 0;
            model.blocks.push_back({container, container == 8 ? nextNumber++ : 1, {}});
            active = static_cast<int>(model.blocks.size()) - 1; return 0;
        case MD_BLOCK_P: case MD_BLOCK_H: case MD_BLOCK_CODE: {
            if (inItem && type == MD_BLOCK_P && active >= 0 && !itemBlocks) { itemBlocks = 1; return 0; }
            if (active >= 0 || inItem) return fail(L"包含多个段落的列表项请在源码模式编辑。");
            if (container && type != MD_BLOCK_P) return fail(L"列表或引用中的标题、代码块请在源码模式编辑。");
            int style = container;
            if (type == MD_BLOCK_H) style = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
            if (type == MD_BLOCK_CODE) {
                if (static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->info.size) return fail(L"带语言或属性的代码块请在源码模式编辑，以保留代码块信息。");
                style = 10;
            }
            model.blocks.push_back({style, container == 8 ? nextNumber++ : 1, {}});
            active = static_cast<int>(model.blocks.size()) - 1; return 0;
        }
        case MD_BLOCK_HTML: return fail(L"HTML 内容请在源码模式编辑。");
        case MD_BLOCK_HR: return fail(L"分隔线请在源码模式编辑。");
        default: return fail(L"表格或其他复杂结构请在源码模式编辑。");
        }
    }
    int end(MD_BLOCKTYPE type) {
        if (type == MD_BLOCK_P || type == MD_BLOCK_H || type == MD_BLOCK_CODE) {
            if (type == MD_BLOCK_CODE && active >= 0) {
                auto& rs = model.blocks[active].runs;
                if (!rs.empty() && !rs.back().text.empty() && rs.back().text.back() == L'\v') rs.back().text.pop_back();
            }
            active = -1;
        } else if (type == MD_BLOCK_LI) {
            inItem = false; active = -1;
        } else if (type == MD_BLOCK_UL || type == MD_BLOCK_OL || type == MD_BLOCK_QUOTE) container = 0;
        return 0;
    }
    int span(MD_SPANTYPE type, bool enter) {
        int i = type == MD_SPAN_STRONG ? 0 : type == MD_SPAN_EM ? 1 : type == MD_SPAN_DEL ? 2 : type == MD_SPAN_CODE ? 3 : -1;
        if (i < 0) return fail(L"链接、图片、数学公式或扩展行内语法请在源码模式编辑。");
        if (enter) ++spans[i]; else --spans[i]; return 0;
    }
    int text(MD_TEXTTYPE type, const char* s, unsigned n) {
        if (active < 0) return fail(L"无法安全转换此文档结构。");
        if (inItem) itemBlocks = 1;
        if (type == MD_TEXT_HTML || type == MD_TEXT_LATEXMATH || type == MD_TEXT_NULLCHAR) return fail(L"HTML、公式或无效字符请在源码模式编辑。");
        // VT is RichEdit's native, non-paragraph line break.
        std::wstring value = type == MD_TEXT_SOFTBR ? L"\v" : type == MD_TEXT_BR ? L"\u2028" : type == MD_TEXT_ENTITY ? entity({s, n}) : wide({s, n});
        for (auto& c : value) if (c == L'\n') c = L'\v';
        add(model.blocks[active], std::move(value), flags()); return 0;
    }
};
bool parse(std::string_view markdown, Model& model, std::wstring& reason) {
    if (markdown.size() > 512 * 1024) { reason = L"文档超出原生富文本安全编辑大小。"; return false; }
    wide(markdown); // Validate the whole source, including parser-discarded text.
    if (markdown.find('\0') != std::string_view::npos) { reason = L"文档包含空字符。"; return false; }
    // Reference definitions are omitted from MD4C's event stream. Reject them
    // before parsing so an unrelated edit cannot silently discard definitions.
    for (size_t p = 0; p < markdown.size();) {
        size_t end = markdown.find('\n', p); if (end == std::string_view::npos) end = markdown.size();
        auto line = markdown.substr(p, end - p); size_t i = line.find_first_not_of(" \t");
        if (i != std::string_view::npos && line[i] == '[' && line.find("]:", i) != std::string_view::npos) {
            reason = L"引用链接或脚注定义请在源码模式编辑。"; return false;
        }
        p = end + 1;
    }
    Parser p;
    MD_PARSER callbacks{};
    callbacks.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_LATEXMATHSPANS | MD_FLAG_WIKILINKS;
    callbacks.enter_block = [](MD_BLOCKTYPE t, void* d, void* u) { return static_cast<Parser*>(u)->begin(t, d); };
    callbacks.leave_block = [](MD_BLOCKTYPE t, void*, void* u) { return static_cast<Parser*>(u)->end(t); };
    callbacks.enter_span = [](MD_SPANTYPE t, void*, void* u) { return static_cast<Parser*>(u)->span(t, true); };
    callbacks.leave_span = [](MD_SPANTYPE t, void*, void* u) { return static_cast<Parser*>(u)->span(t, false); };
    callbacks.text = [](MD_TEXTTYPE t, const char* s, unsigned n, void* u) { return static_cast<Parser*>(u)->text(t, s, n); };
    if (md_parse(markdown.data(), static_cast<unsigned>(markdown.size()), &callbacks, &p)) {
        reason = p.reason.empty() ? L"Markdown 解析失败。" : p.reason; return false;
    }
    model = std::move(p.model); return true;
}
void rtfText(std::string& out, std::wstring_view text) {
    for (wchar_t c : text) {
        if (c == L'\v') out += "\\line ";
        else if (c == L'\t') out += "\\tab ";
        else if (c == L'\\' || c == L'{' || c == L'}') { out += '\\'; out += static_cast<char>(c); }
        else if (c >= 32 && c < 127) out += static_cast<char>(c);
        else out += "\\u" + std::to_string(static_cast<short>(c)) + "?";
    }
}
std::string rtf(const Model& m, bool dark) {
    std::string out = "{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0 Segoe UI;}{\\f1 Consolas;}}{\\colortbl ;";
    out += dark ? "\\red224\\green227\\blue233;\\red41\\green45\\blue54;" : "\\red32\\green38\\blue46;\\red239\\green242\\blue246;";
    out += "}\\viewkind4\\cf1\\fs24 ";
    for (size_t i = 0; i < m.blocks.size(); ++i) {
        auto& b = m.blocks[i];
        if (i) out += "\\par\n";
        out += "\\pard\\ql\\outlinelevel9\\sa120\\sl320\\slmult1 ";
        if (b.style >= 1 && b.style <= 6) out += "\\outlinelevel" + std::to_string(b.style - 1) + " ";
        if (b.style == 7) out += "{\\*\\pn\\pnlvlblt\\pnf0\\pnindent360{\\pntxtb\\'b7}}\\li360\\fi-180 ";
        if (b.style == 8) out += "{\\*\\pn\\pnlvlbody\\pndec\\pnstart" + std::to_string(b.number) + "{\\pntxta.}}\\li360\\fi-180 ";
        if (b.style == 9) out += "\\li360 ";
        if (b.style == 10) out += "\\li420 ";
        for (auto& run : b.runs) {
            out += "{\\b" + std::to_string(bool(run.flags & Bold)) + "\\i" + std::to_string(bool(run.flags & Italic)) + "\\strike" + std::to_string(bool(run.flags & Strike));
            out += (run.flags & Code) || b.style == 10 ? "\\f1" : "\\f0";
            out += "\\fs" + std::to_string(b.style >= 1 && b.style <= 6 ? 44 - b.style * 3 : 24);
            if (run.flags & Code) out += "\\highlight2";
            out += ' '; rtfText(out, run.text); out += '}';
        }
    }
    out += '}'; return out;
}
struct Stream { const std::string* text; size_t pos = 0; };
DWORD CALLBACK streamIn(DWORD_PTR cookie, LPBYTE buffer, LONG count, LONG* written) {
    auto& s = *reinterpret_cast<Stream*>(cookie);
    size_t n = std::min(static_cast<size_t>(count), s.text->size() - s.pos);
    std::copy_n(s.text->data() + s.pos, n, buffer); s.pos += n; *written = static_cast<LONG>(n); return 0;
}
void select(HWND rich, LONG begin, LONG end) { CHARRANGE range{begin, end}; SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range)); }
struct Preserve {
    HWND rich; CHARRANGE range{}; POINT scroll{};
    explicit Preserve(HWND h) : rich(h) { SendMessageW(h, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&range)); SendMessageW(h, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll)); SendMessageW(h, WM_SETREDRAW, FALSE, 0); }
    ~Preserve() { SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range)); SendMessageW(rich, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll)); SendMessageW(rich, WM_SETREDRAW, TRUE, 0); InvalidateRect(rich, nullptr, FALSE); }
};
struct SuspendUndo {
    ITextDocument* document = nullptr;
    bool suspended = false;
    explicit SuspendUndo(HWND rich) {
        IUnknown* object = nullptr;
        if (SendMessageW(rich, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&object)) && object) {
            // MinGW's UUID import library omits this TOM IID.
            static constexpr IID documentId{0x8cc497c0, 0xa1df, 0x11ce, {0x80, 0x98, 0x00, 0xaa, 0x00, 0x47, 0xbe, 0x5d}};
            object->QueryInterface(documentId, reinterpret_cast<void**>(&document)); object->Release();
            if (document) { LONG count = 0; suspended = SUCCEEDED(document->Undo(tomSuspend, &count)); }
        }
    }
    ~SuspendUndo() { if (document) { if (suspended) { LONG count = 0; document->Undo(tomResume, &count); } document->Release(); } }
};
CHARFORMAT2W character(HWND rich, LONG begin, LONG end) {
    select(rich, begin, end); CHARFORMAT2W cf{}; cf.cbSize = sizeof(cf);
    SendMessageW(rich, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf)); return cf;
}
constexpr DWORD charMask = CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_BACKCOLOR | CFM_UNDERLINE | CFM_LINK | CFM_HIDDEN | CFM_PROTECTED | CFM_SUPERSCRIPT;
bool uniform(const CHARFORMAT2W& c) { return (c.dwMask & charMask) == charMask; }
bool flagsOf(const CHARFORMAT2W& c, int block, unsigned& flags, std::wstring& error) {
    if (c.dwEffects & (CFE_UNDERLINE | CFE_LINK | CFE_HIDDEN | CFE_PROTECTED | CFE_SUPERSCRIPT | CFE_SUBSCRIPT)) { error = L"包含下划线、链接、隐藏文字或其他无法保存为 Markdown 的格式。"; return false; }
    flags = (c.dwEffects & CFE_BOLD ? Bold : 0) | (c.dwEffects & CFE_ITALIC ? Italic : 0) | (c.dwEffects & CFE_STRIKEOUT ? Strike : 0);
    if (!(c.dwEffects & CFE_AUTOBACKCOLOR) && (c.crBackColor == InlineLight || c.crBackColor == InlineDark) && block != 10) flags |= Code;
    if (!(c.dwEffects & CFE_AUTOBACKCOLOR) && c.crBackColor != InlineLight && c.crBackColor != InlineDark && c.crBackColor != HardBreak) { error = L"自定义文字背景色无法保存为 Markdown。"; return false; }
    if (block == 10 && flags) { error = L"代码块内的粗体或斜体无法保存；请先清除格式。"; return false; }
    return true;
}
bool read(HWND rich, Model& model, std::wstring& error) {
    GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE, 1200};
    LONG n = static_cast<LONG>(SendMessageW(rich, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0));
    if (n < 0 || n > 512 * 1024) { error = L"富文本长度超出安全范围。"; return false; }
    std::wstring text(n + 2, L'\0'); GETTEXTEX request{static_cast<DWORD>(text.size() * sizeof(wchar_t)), GT_DEFAULT, 1200, nullptr, nullptr};
    LONG got = static_cast<LONG>(SendMessageW(rich, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&request), reinterpret_cast<LPARAM>(text.data())));
    if (got < 0 || got > n + 1) { error = L"读取原生编辑器失败。"; return false; } text.resize(got);
    utf8(text);
    if (text.find(L'\ufffc') != std::wstring::npos) { error = L"嵌入对象无法保存为 Markdown。"; return false; }
    for (LONG start = 0; start <= got;) {
        auto found = text.find(L'\r', start); LONG end = found == std::wstring::npos ? got : static_cast<LONG>(found);
        select(rich, start, start); PARAFORMAT2 p{}; p.cbSize = sizeof(p); SendMessageW(rich, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&p));
        Block b;
        if (p.wAlignment != PFA_LEFT || p.cTabCount || p.dxRightIndent || (p.wEffects & PFE_TABLE)) { error = L"表格、对齐或自定义制表位格式无法保存为 Markdown。"; return false; }
        if (p.sStyle != 0 && p.sStyle != -1 && (p.sStyle < 100 || p.sStyle > 110)) { error = L"未知原生段落样式无法保存为 Markdown。"; return false; }
        if (p.wNumbering == PFN_BULLET) b.style = 7;
        else if (p.wNumbering == PFN_ARABIC) { b.style = 8; b.number = p.wNumberingStart ? p.wNumberingStart : 1; }
        else if (p.wNumbering) { error = L"此编号样式无法保存为 Markdown。"; return false; }
        else if (p.sStyle >= 101 && p.sStyle <= 106) b.style = p.sStyle - 100;
        else if (p.dxStartIndent == QuoteIndent && !p.dxOffset) b.style = 9;
        else if (p.dxStartIndent == CodeIndent && !p.dxOffset) b.style = 10;
        else if (p.dxStartIndent || p.dxOffset) { error = L"自定义缩进或嵌套内容请使用源码编辑。"; return false; }
        if ((b.style == 7 || b.style == 8) && (p.dxStartIndent != 360 || p.dxOffset != -180)) { error = L"嵌套列表缩进请使用源码编辑。"; return false; }
        if (b.style >= 1 && b.style <= 6 && (p.dxStartIndent || p.dxOffset)) { error = L"自定义标题缩进请使用源码编辑。"; return false; }
        if (b.style == 8 && (p.wNumberingStyle & ~PFNS_NEWNUMBER) != PFNS_PERIOD) { error = L"此编号标点格式无法保存为 Markdown。"; return false; }
        if (b.style == 8 && !model.blocks.empty() && model.blocks.back().style == 8 && !(p.wNumberingStyle & PFNS_NEWNUMBER)) b.number = model.blocks.back().number + 1;
        for (LONG at = start; at < end;) {
            // Binary search native format runs. Text is retrieved once for the
            // whole document; no per-character document copies or COM lifetime.
            LONG low = at + 1, high = end;
            while (low < high) {
                LONG mid = low + (high - low + 1) / 2;
                if (uniform(character(rich, at, mid))) low = mid; else high = mid - 1;
            }
            auto c = character(rich, at, low); unsigned flags;
            if (!flagsOf(c, b.style, flags, error)) return false;
            auto part = text.substr(at, low - at);
            if (!(c.dwEffects & CFE_AUTOBACKCOLOR) && c.crBackColor == HardBreak) for (auto& ch : part) if (ch == L'\v') ch = L'\u2028';
            add(b, std::move(part), flags); at = low;
        }
        model.blocks.push_back(std::move(b));
        if (end == got) break; start = end + 1;
    }
    return true;
}
bool whitespace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\v' || c == L'\u2028'; }
std::vector<std::pair<wchar_t, unsigned>> normalized(const Block& block) {
    std::vector<std::pair<wchar_t, unsigned>> result;
    for (auto& run : block.runs) for (auto c : run.text) result.emplace_back(c, whitespace(c) && !(run.flags & Code) ? 0 : run.flags);
    return result;
}
std::string escaped(std::wstring_view text) {
    std::wstring out;
    for (auto c : text) {
        if (c == L'\v') out += L'\n';
        else if (c == L'\u2028') out += L"  \n";
        else {
            if ((c >= L'!' && c <= L'/') || (c >= L':' && c <= L'@') || (c >= L'[' && c <= L'`') || (c >= L'{' && c <= L'~')) out += L'\\';
            out += c;
        }
    }
    return utf8(out);
}
std::string inlineText(const Block& block) {
    auto chars = normalized(block); std::string out;
    std::vector<unsigned> stack;
    const auto marker = [](unsigned f) { return f == Bold ? "**" : f == Italic ? "*" : "~~"; };
    for (size_t at = 0; at < chars.size();) {
        unsigned flags = chars[at].second;
        size_t end = at + 1; while (end < chars.size() && chars[end].second == flags) ++end;
        std::vector<unsigned> next; for (auto f : {Strike, Bold, Italic}) if (flags & f) next.push_back(f);
        size_t common = 0; while (common < stack.size() && common < next.size() && stack[common] == next[common]) ++common;
        for (size_t i = stack.size(); i > common; --i) out += marker(stack[i - 1]);
        for (size_t i = common; i < next.size(); ++i) out += marker(next[i]);
        stack = std::move(next);
        std::wstring part; for (size_t i = at; i < end; ++i) part += chars[i].first;
        if (flags & Code) {
            size_t longest = 0, current = 0; for (auto c : part) { current = c == L'`' ? current + 1 : 0; longest = std::max(longest, current); }
            std::string fence(longest + 1, '`'); bool allSpaces = part.find_first_not_of(L' ') == std::wstring::npos;
            out += fence; if (!allSpaces) out += ' '; out += utf8(part); if (!allSpaces) out += ' '; out += fence;
        } else out += escaped(part);
        at = end;
    }
    for (auto i = stack.rbegin(); i != stack.rend(); ++i) out += marker(*i);
    return out;
}
std::string serialize(const Model& model) {
    std::string out;
    for (size_t i = 0; i < model.blocks.size(); ++i) {
        auto& b = model.blocks[i];
        if (i) out += b.style == 7 && model.blocks[i - 1].style == 7 ? "\n" : b.style == 8 && model.blocks[i - 1].style == 8 ? "\n" : "\n\n";
        if (b.style == 10) {
            std::wstring code; for (auto& run : b.runs) code += run.text;
            for (auto& c : code) if (c == L'\v' || c == L'\u2028') c = L'\n';
            size_t longest = 2, current = 0; for (auto c : code) { current = c == L'`' ? current + 1 : 0; longest = std::max(longest, current); }
            std::string fence(longest + 1, '`'); out += fence + "\n" + utf8(code) + "\n" + fence; continue;
        }
        std::string value = inlineText(b), prefix;
        if (b.style >= 1 && b.style <= 6) prefix = std::string(b.style, '#') + " ";
        else if (b.style == 7) prefix = "- ";
        else if (b.style == 8) prefix = std::to_string(b.number) + ". ";
        else if (b.style == 9) prefix = "> ";
        out += prefix;
        for (char c : value) { out += c; if (c == '\n') out += b.style == 9 ? "> " : b.style == 7 || b.style == 8 ? std::string(prefix.size(), ' ') : ""; }
    }
    if (!out.empty()) out += '\n'; return out;
}
void trimEmpty(Model& model) {
    // RichEdit always has an empty final paragraph. Extra blank paragraphs in
    // Markdown have no rendered semantics, so canonicalize them consistently.
    model.blocks.erase(std::remove_if(model.blocks.begin(), model.blocks.end(), [](auto& b) { return b.style == 0 && std::all_of(b.runs.begin(), b.runs.end(), [](auto& r) { return r.text.empty(); }); }), model.blocks.end());
}
bool same(Model a, Model b) {
    trimEmpty(a); trimEmpty(b);
    if (a.blocks.size() != b.blocks.size()) return false;
    for (size_t i = 0; i < a.blocks.size(); ++i) {
        if (a.blocks[i].style != b.blocks[i].style || normalized(a.blocks[i]) != normalized(b.blocks[i])) return false;
        if (a.blocks[i].style == 8 && a.blocks[i].number != b.blocks[i].number) return false;
    }
    return true;
}
void paragraphFormat(HWND rich, int style) {
    PARAFORMAT2 p{}; p.cbSize = sizeof(p);
    p.dwMask = PFM_STYLE | PFM_NUMBERING | PFM_NUMBERINGSTART | PFM_NUMBERINGSTYLE | PFM_NUMBERINGTAB | PFM_STARTINDENT | PFM_OFFSET | PFM_ALIGNMENT;
    // sStyle is explicitly stored by RichEdit even for custom styles. Unlike
    // font sizes or bOutlineLevel, it survives theme, zoom and font fallback.
    p.sStyle = static_cast<SHORT>(100 + style); p.wAlignment = PFA_LEFT;
    if (style == 7 || style == 8) { p.wNumbering = style == 7 ? PFN_BULLET : PFN_ARABIC; p.wNumberingStart = 1; p.wNumberingStyle = PFNS_PERIOD; p.wNumberingTab = 360; p.dxStartIndent = 360; p.dxOffset = -180; }
    if (style == 9) p.dxStartIndent = QuoteIndent;
    if (style == 10) p.dxStartIndent = CodeIndent;
    SendMessageW(rich, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&p));
}
}
RichLoad load(HWND rich, std::string_view markdown, bool dark) {
    try {
        if (!IsWindow(rich)) return {false, L"原生编辑器尚未创建。"};
        Model model; std::wstring reason;
        if (!parse(markdown, model, reason)) return {false, reason};
        auto data = rtf(model, dark); Stream cookie{&data}; EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&cookie), 0, streamIn};
        SendMessageW(rich, WM_SETREDRAW, FALSE, 0);
        SendMessageW(rich, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream));
        SendMessageW(rich, WM_SETREDRAW, TRUE, 0); InvalidateRect(rich, nullptr, TRUE);
        if (stream.dwError) return {false, L"原生富文本导入失败。"};
        // Assign paragraph metadata through the native API; RTF numbering
        // compatibility varies between Windows RichEdit releases.
        LONG pos = 0;
        for (auto& b : model.blocks) {
            select(rich, pos, pos); paragraphFormat(rich, b.style);
            if (b.style == 8) { PARAFORMAT2 p{}; p.cbSize = sizeof(p); p.dwMask = PFM_NUMBERINGSTART; p.wNumberingStart = static_cast<WORD>(b.number); SendMessageW(rich, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&p)); }
            for (auto& run : b.runs) {
                for (size_t i = 0; i < run.text.size(); ++i) if (run.text[i] == L'\u2028') {
                    select(rich, pos + static_cast<LONG>(i), pos + static_cast<LONG>(i + 1));
                    CHARFORMAT2W c{}; c.cbSize = sizeof(c); c.dwMask = CFM_BACKCOLOR; c.crBackColor = HardBreak;
                    SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&c));
                }
                pos += static_cast<LONG>(run.text.size());
            }
            ++pos;
        }
        Model check; { Preserve keep(rich); if (!read(rich, check, reason) || !same(model, check)) return {false, reason.empty() ? L"原生控件无法完整保留此文档格式，请使用源码模式。" : reason}; }
        SendMessageW(rich, EM_SETBKGNDCOLOR, 0, dark ? RGB(25, 29, 36) : RGB(255, 255, 255));
        select(rich, 0, 0); SendMessageW(rich, EM_SETMODIFY, FALSE, 0); SendMessageW(rich, EM_EMPTYUNDOBUFFER, 0, 0);
        return {true, {}};
    } catch (...) { return {false, L"文本编码无效或原生富文本转换失败。"}; }
}
bool save(HWND rich, std::string& markdown, std::wstring& error) {
    error.clear();
    try {
        if (!IsWindow(rich)) { error = L"原生编辑器尚未创建。"; return false; }
        Preserve keep(rich); Model model;
        if (!read(rich, model, error)) return false;
        auto output = serialize(model); Model check;
        if (!parse(output, check, error) || !same(model, check)) { if (error.empty()) error = L"此格式组合无法安全保存为 Markdown，请调整格式或使用源码模式。"; return false; }
        markdown = std::move(output); return true;
    } catch (...) { error = L"文本编码无效或原生富文本导出失败。"; return false; }
}
void toggleInline(HWND rich, int style) {
    CHARFORMAT2W c{}; c.cbSize = sizeof(c); SendMessageW(rich, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&c));
    if (style >= 1 && style <= 3) {
        DWORD flag = style == 1 ? CFM_BOLD : style == 2 ? CFM_ITALIC : CFM_STRIKEOUT;
        bool on = (c.dwMask & flag) && (c.dwEffects & flag); c.dwMask = flag; c.dwEffects = on ? 0 : flag;
    } else if (style == 4) {
        bool on = (c.dwMask & CFM_BACKCOLOR) && !(c.dwEffects & CFE_AUTOBACKCOLOR) && (c.crBackColor == InlineLight || c.crBackColor == InlineDark);
        c.dwMask = CFM_FACE | CFM_BACKCOLOR; wcscpy_s(c.szFaceName, on ? L"Segoe UI" : L"Consolas");
        c.dwEffects = on ? CFE_AUTOBACKCOLOR : 0; c.crBackColor = InlineLight;
    } else return;
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&c));
}
void setBlock(HWND rich, int style) {
    if (style < 0 || style > 10) return;
    paragraphFormat(rich, style);
    // Format whole logical paragraphs, including an empty paragraph's typing
    // format, without allowing display font size to become semantic metadata.
    Preserve keep(rich);
    GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE, 1200};
    LONG n = static_cast<LONG>(SendMessageW(rich, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0));
    if (n < 0 || n > 512 * 1024) return;
    std::wstring text(n + 2, L'\0'); GETTEXTEX request{static_cast<DWORD>(text.size() * sizeof(wchar_t)), GT_DEFAULT, 1200, nullptr, nullptr};
    LONG got = static_cast<LONG>(SendMessageW(rich, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&request), reinterpret_cast<LPARAM>(text.data())));
    if (got < 0) return; text.resize(got);
    size_t from = static_cast<size_t>(std::clamp(keep.range.cpMin, 0L, got));
    size_t to = static_cast<size_t>(std::clamp(keep.range.cpMax, 0L, got));
    auto left = from ? text.rfind(L'\r', from - 1) : std::wstring::npos;
    auto right = text.find(L'\r', to > from ? to - 1 : to);
    select(rich, left == std::wstring::npos ? 0 : static_cast<LONG>(left + 1), right == std::wstring::npos ? got : static_cast<LONG>(right));
    CHARFORMAT2W c{}; c.cbSize = sizeof(c); c.dwMask = CFM_SIZE; c.yHeight = (style >= 1 && style <= 6 ? 44 - style * 3 : 24) * 10;
    if (style == 10) {
        c.dwMask |= CFM_FACE | CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_BACKCOLOR;
        c.dwEffects = CFE_AUTOBACKCOLOR; wcscpy_s(c.szFaceName, L"Consolas");
    }
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&c));
}
void restyle(HWND rich, bool dark) {
    SuspendUndo undo(rich);
    if (!undo.suspended) return; // Do not damage the existing undo history.
    BOOL modified = static_cast<BOOL>(SendMessageW(rich, EM_GETMODIFY, 0, 0));
    Preserve keep(rich); select(rich, 0, -1);
    CHARFORMAT2W c{}; c.cbSize = sizeof(c); c.dwMask = CFM_COLOR; c.crTextColor = dark ? RGB(224, 227, 233) : RGB(32, 38, 46);
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&c));
    GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE, 1200};
    LONG n = static_cast<LONG>(SendMessageW(rich, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0));
    for (LONG at = 0; at < n;) {
        LONG low = at + 1, high = n;
        while (low < high) { LONG mid = low + (high - low + 1) / 2; auto run = character(rich, at, mid); if (run.dwMask & CFM_BACKCOLOR) low = mid; else high = mid - 1; }
        auto run = character(rich, at, low);
        if (!(run.dwEffects & CFE_AUTOBACKCOLOR) && (run.crBackColor == InlineLight || run.crBackColor == InlineDark)) {
            run.dwMask = CFM_BACKCOLOR; run.dwEffects = 0; run.crBackColor = dark ? InlineDark : InlineLight;
            SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&run));
        }
        at = low;
    }
    SendMessageW(rich, EM_SETBKGNDCOLOR, 0, dark ? RGB(25, 29, 36) : RGB(255, 255, 255));
    SendMessageW(rich, EM_SETMODIFY, modified, 0);
}
bool insertCodeLineBreak(HWND rich) {
    if (!IsWindow(rich) || (GetWindowLongPtrW(rich, GWL_STYLE) & ES_READONLY)) return false;
    PARAFORMAT2 p{}; p.cbSize = sizeof(p);
    SendMessageW(rich, EM_GETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&p));
    if (!(p.dwMask & PFM_STARTINDENT) || p.dxStartIndent != CodeIndent || p.dxOffset || p.wNumbering) return false;
    IUnknown* object = nullptr; ITextDocument* document = nullptr;
    if (SendMessageW(rich, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&object)) && object) {
        static constexpr IID documentId{0x8cc497c0, 0xa1df, 0x11ce, {0x80, 0x98, 0x00, 0xaa, 0x00, 0x47, 0xbe, 0x5d}};
        object->QueryInterface(documentId, reinterpret_cast<void**>(&document)); object->Release();
    }
    if (!document) return false;
    if (FAILED(document->BeginEditCollection())) { document->Release(); return false; }
    CHARRANGE before{}; SendMessageW(rich, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&before));
    GETTEXTLENGTHEX lengthQuery{GTL_NUMCHARS | GTL_PRECISE, 1200};
    const LRESULT lengthBefore = SendMessageW(rich, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&lengthQuery), 0);
    std::wstring selectedBefore(static_cast<size_t>(before.cpMax - before.cpMin) + 1, L'\0');
    TEXTRANGEW selectedRange{before, selectedBefore.data()};
    SendMessageW(rich, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&selectedRange));
    CHARFORMAT2W characterBefore{}; characterBefore.cbSize = sizeof(characterBefore);
    SendMessageW(rich, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&characterBefore));
    // RichEdit normalizes EM_REPLACESEL's VT into a paragraph break. RTF's
    // explicit line control inserts the native soft-line-break character.
    const std::string line = "{\\rtf1\\ansi\\line }";
    Stream cookie{&line}; EDITSTREAM input{reinterpret_cast<DWORD_PTR>(&cookie), 0, streamIn};
    SendMessageW(rich, EM_STREAMIN, SF_RTF | SFF_SELECTION, reinterpret_cast<LPARAM>(&input));
    // A standalone RTF fragment may reset paragraph/typing defaults. Restore
    // the explicit code metadata within the same user undo action.
    paragraphFormat(rich, 10);
    characterBefore.dwMask &= CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_BACKCOLOR | CFM_COLOR;
    SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&characterBefore));
    document->EndEditCollection(); document->Release();
    wchar_t inserted[2]{}; TEXTRANGEW range{{before.cpMin, before.cpMin + 1}, inserted};
    SendMessageW(rich, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&range));
    if (input.dwError || inserted[0] != L'\v') {
        const LRESULT lengthAfter = SendMessageW(rich, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&lengthQuery), 0);
        std::wstring selectedAfter(selectedBefore.size(), L'\0');
        selectedRange.lpstrText = selectedAfter.data();
        SendMessageW(rich, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&selectedRange));
        // Never undo an earlier user edit when streaming made no text change.
        if (lengthAfter != lengthBefore || selectedAfter != selectedBefore) SendMessageW(rich, EM_UNDO, 0, 0);
        return false;
    }
    return true;
}
}
