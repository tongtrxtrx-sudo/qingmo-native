#include "preview.hpp"
#include "md4c.h"
extern "C" {
#include "entity.h"
}

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace md {
namespace {

void appendCodepoint(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp < 128) {
        if (cp == '\\' || cp == '{' || cp == '}') out += '\\';
        if (cp == '\n') out += "\\line ";
        else if (cp == '\t') out += "\\tab ";
        else if (cp == '\r') {} // CRLF uses the LF; bare CR is normalized by MD4C.
        else if (cp < 32) out += "\\u-3?";
        else out += static_cast<char>(cp);
        return;
    }
    const auto unit = [&](uint32_t value) {
        out += "\\u";
        out += std::to_string(value < 32768 ? static_cast<int>(value) : static_cast<int>(value) - 65536);
        out += '?';
    };
    if (cp <= 0xffff) unit(cp);
    else { cp -= 0x10000; unit(0xd800 + (cp >> 10)); unit(0xdc00 + (cp & 1023)); }
}

void appendText(std::string& out, std::string_view text, bool fontName = false) {
    if (fontName) {
        // RichEdit treats Unicode controls inside fonttbl as body text. Font
        // entries instead use UTF-8 hex bytes with cpg65001. Semicolons delimit
        // names even after decoding, so normalize them and controls to spaces.
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char ch : text) {
            if (ch < 32 || ch == 127 || ch == ';') ch = ' ';
            if (ch >= 128 || ch == '\\' || ch == '{' || ch == '}') {
                out += "\\'"; out += hex[ch >> 4]; out += hex[ch & 15];
            } else out += static_cast<char>(ch);
        }
        return;
    }
    for (size_t i = 0; i < text.size();) {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (ch < 128) {
            appendCodepoint(out, ch);
            ++i; continue;
        }
        const unsigned count = ch >= 0xc2 && ch <= 0xdf ? 2 : ch >= 0xe0 && ch <= 0xef ? 3 : ch >= 0xf0 && ch <= 0xf4 ? 4 : 0;
        uint32_t cp = count ? ch & ((1u << (7 - count)) - 1u) : 0;
        bool valid = count && i + count <= text.size();
        for (unsigned j = 1; valid && j < count; ++j) {
            const auto tail = static_cast<unsigned char>(text[i + j]);
            valid = (tail & 0xc0) == 0x80;
            cp = (cp << 6) | (tail & 0x3f);
        }
        valid = valid && cp >= (count == 2 ? 0x80u : count == 3 ? 0x800u : 0x10000u)
            && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
        appendCodepoint(out, valid ? cp : 0xfffd);
        i += valid ? count : 1;
    }
}

void appendEntity(std::string& out, std::string_view text) {
    if (text.size() > 3 && text[1] == '#') {
        size_t i = 2;
        const bool hex = text[i] == 'x' || text[i] == 'X';
        if (hex) ++i;
        uint32_t cp = 0;
        for (; i + 1 < text.size(); ++i) {
            const char c = text[i];
            const unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
            const unsigned base = hex ? 16 : 10;
            if (digit >= base || cp > (0x10ffffu - digit) / base) { cp = 0xfffd; break; }
            cp = cp * base + digit;
        }
        appendCodepoint(out, cp);
    } else if (const ENTITY* entity = entity_lookup(text.data(), text.size())) {
        appendCodepoint(out, entity->codepoints[0]);
        if (entity->codepoints[1]) appendCodepoint(out, entity->codepoints[1]);
    } else appendText(out, text);
}

struct List { bool ordered; bool tight; uint64_t next; };
struct Item { bool used = false; bool task = false; bool checked = false; uint64_t number = 1; };

class Renderer {
public:
    std::string out;
    explicit Renderer(const PreviewOptions& options)
        : width(std::clamp(options.widthTwips, 1200, 30000)),
          fontSize(std::clamp(options.fontSizeHalfPoints, 18, 56)),
          lineHeight(std::clamp(options.lineHeightPercent, 120, 220)),
          paragraphSpacing(std::clamp(options.paragraphSpacingTwips, 60, 480)), reading(options.reading) {
        out = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1\n{\\fonttbl{\\f0\\fnil\\fcharset0\\cpg65001 ";
        appendText(out, options.fontFamily.empty() ? std::string_view("Microsoft YaHei")
            : std::string_view(options.fontFamily).substr(0, 256), true);
        out += ";}{\\f1\\fmodern\\fcharset0 Consolas;}}\n";
        // Color indexes: text, muted, accent, code background, rule, table header.
        out += options.dark
            ? "{\\colortbl;\\red219\\green228\\blue219;\\red154\\green169\\blue159;\\red145\\green185\\blue158;\\red45\\green60\\blue50;\\red53\\green67\\blue58;\\red45\\green60\\blue50;}\n"
            : "{\\colortbl;\\red45\\green54\\blue48;\\red127\\green137\\blue126;\\red61\\green122\\blue96;\\red230\\green237\\blue228;\\red220\\green226\\blue216;\\red230\\green237\\blue228;}\n";
        out += "\\viewkind4\\paperw" + std::to_string(width + 480) + "\\margl240\\margr240\\f0\\fs" + std::to_string(fontSize) + "\\cf1\n";
    }

    void closeParagraph() {
        if (!paragraph || cell) return;
        out += "\\par}\n";
        paragraph = false;
    }

    void beginParagraph(const std::string& style = {}) {
        if (paragraph) return;
        paragraph = true;
        const int indent = indentation();
        const bool marker = !items.empty() && !items.back().used;
        const bool tight = !lists.empty() && lists.back().tight;
        out += "{\\pard\\plain\\f0\\fs" + std::to_string(fontSize) + "\\cf1" + lineSpacing(lineHeight) + "\\sa";
        out += std::to_string(tight ? std::max(60, paragraphSpacing / 2) : paragraphSpacing);
        out += "\\li" + std::to_string(indent) + "\\ri0";
        if (quoteDepth) out += "\\brdrl\\brdrs\\brdrw20\\brdrcf5\\brsp120\\cf2";
        if (marker) out += "\\fi-300\\tx" + std::to_string(indent);
        out += style + " ";
        if (marker) {
            Item& item = items.back();
            item.used = true;
            if (item.task) appendCodepoint(out, item.checked ? 0x2611 : 0x2610);
            else if (!lists.empty() && lists.back().ordered) out += std::to_string(item.number) + ".";
            else appendCodepoint(out, 0x2022);
            out += "\\tab ";
        }
    }

    void enterBlock(MD_BLOCKTYPE type, void* detail) {
        switch (type) {
        case MD_BLOCK_DOC: break;
        case MD_BLOCK_QUOTE: closeParagraph(); ++quoteDepth; break;
        case MD_BLOCK_UL: {
            if (!items.empty() && !items.back().used) beginParagraph();
            closeParagraph(); const auto& d = *static_cast<MD_BLOCK_UL_DETAIL*>(detail);
            lists.push_back({false, d.is_tight != 0, 1}); break;
        }
        case MD_BLOCK_OL: {
            if (!items.empty() && !items.back().used) beginParagraph();
            closeParagraph(); const auto& d = *static_cast<MD_BLOCK_OL_DETAIL*>(detail);
            lists.push_back({true, d.is_tight != 0, d.start}); break;
        }
        case MD_BLOCK_LI: {
            closeParagraph(); const auto& d = *static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            items.push_back({false, d.is_task != 0, d.is_task && (d.task_mark == 'x' || d.task_mark == 'X'), lists.empty() ? 1 : lists.back().next++});
            break;
        }
        case MD_BLOCK_P: closeParagraph(); beginParagraph(); break;
        case MD_BLOCK_H: {
            closeParagraph(); const unsigned level = std::clamp(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level, 1u, 6u);
            static const int scale[] = {180, 150, 128, 114, 106, 100};
            const int before = paragraphSpacing + (level <= 2 ? 120 : 60) + (reading ? 80 : 0);
            beginParagraph("\\b\\keepn\\sb" + std::to_string(before) + "\\sa" + std::to_string(std::max(90, paragraphSpacing * 2 / 3))
                + "\\fs" + std::to_string((fontSize * scale[level - 1] + 50) / 100) + lineSpacing(std::min(lineHeight, 140))); break;
        }
        case MD_BLOCK_CODE:
            closeParagraph();
            beginParagraph("\\f1\\fs" + std::to_string(fontSize - 2) + lineSpacing(std::min(lineHeight, 150))
                + "\\sb90\\sa" + std::to_string(paragraphSpacing) + "\\cbpat4"); break;
        case MD_BLOCK_HR:
            closeParagraph(); beginParagraph("\\sb100\\sa160\\brdrb\\brdrs\\brdrw15\\brdrcf5");
            out += "\\fs4 "; closeParagraph(); break;
        case MD_BLOCK_TABLE:
            if (!items.empty() && !items.back().used) beginParagraph();
            closeParagraph(); columns = std::max(1u, static_cast<MD_BLOCK_TABLE_DETAIL*>(detail)->col_count); break;
        case MD_BLOCK_TR: column = 0; break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: startCell(type == MD_BLOCK_TH, static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align); break;
        default: break;
        }
    }

    void leaveBlock(MD_BLOCKTYPE type) {
        switch (type) {
        case MD_BLOCK_DOC: closeParagraph(); break;
        case MD_BLOCK_P: case MD_BLOCK_H: closeParagraph(); break;
        case MD_BLOCK_CODE:
            if (out.size() >= 6 && out.compare(out.size() - 6, 6, "\\line ") == 0) out.resize(out.size() - 6);
            closeParagraph(); break;
        case MD_BLOCK_LI:
            if (!items.empty() && !items.back().used) beginParagraph();
            closeParagraph(); if (!items.empty()) items.pop_back(); break;
        case MD_BLOCK_UL: case MD_BLOCK_OL: closeParagraph(); if (!lists.empty()) lists.pop_back(); break;
        case MD_BLOCK_QUOTE: closeParagraph(); if (quoteDepth) --quoteDepth; break;
        case MD_BLOCK_TH: case MD_BLOCK_TD:
            out += "\\cell}\n"; cell = false; paragraph = false; ++column;
            if (column == chunkEnd) out += "\\row}\n";
            break;
        case MD_BLOCK_TABLE: columns = 0; break;
        default: break;
        }
    }

    void enterSpan(MD_SPANTYPE type) {
        beginParagraph();
        switch (type) {
        case MD_SPAN_EM: out += "{\\i "; break;
        case MD_SPAN_STRONG: out += "{\\b "; break;
        case MD_SPAN_DEL: out += "{\\strike "; break;
        case MD_SPAN_CODE: out += "{\\f1\\fs" + std::to_string(fontSize - 2) + "\\highlight4 "; break;
        case MD_SPAN_A: out += "{\\cf3\\ul "; break;
        case MD_SPAN_IMG: out += "{\\cf2 [Image: "; break;
        default: out += '{'; break;
        }
    }
    void leaveSpan(MD_SPANTYPE type) { if (type == MD_SPAN_IMG) out += ']'; out += '}'; }

    void text(MD_TEXTTYPE type, std::string_view value) {
        beginParagraph();
        if (type == MD_TEXT_ENTITY) appendEntity(out, value);
        else if (type == MD_TEXT_NULLCHAR) appendCodepoint(out, 0xfffd);
        else if (type == MD_TEXT_BR) out += "\\line ";
        else if (type == MD_TEXT_SOFTBR) out += ' ';
        else appendText(out, value);
    }

private:
    int width;
    int fontSize;
    int lineHeight;
    int paragraphSpacing;
    bool reading;
    unsigned quoteDepth = 0;
    bool paragraph = false;
    bool cell = false;
    std::vector<List> lists;
    std::vector<Item> items;
    unsigned columns = 0, column = 0, chunkEnd = 0;

    static std::string lineSpacing(int percent) {
        // RTF multiple spacing uses 240 units per single line; RichEdit exposes
        // the imported value through PARAFORMAT2 in twentieths of a line.
        return "\\sl" + std::to_string((percent * 240 + 50) / 100) + "\\slmult1";
    }

    int indentation() const {
        // Deep input must not push text outside the viewport or overflow twips.
        return std::min(width / 2, static_cast<int>(std::min<size_t>(items.size(), 32)) * 360 + static_cast<int>(std::min(quoteDepth, 32u)) * 240);
    }

    void startCell(bool header, MD_ALIGN align) {
        const int left = indentation();
        const int available = std::max(600, width - left);
        // RichEdit tables have practical column limits. Wrap wide logical rows
        // into physical rows, preserving every cell and keeping columns legible.
        const unsigned perRow = static_cast<unsigned>(std::clamp(available / 1100, 1, 8));
        if (column % perRow == 0) {
            const unsigned count = std::min(perRow, columns - column);
            chunkEnd = column + count;
            out += "{\\trowd\\trgaph90\\trleft" + std::to_string(left);
            for (unsigned i = 1; i <= count; ++i) {
                out += "\\clbrdrt\\brdrs\\brdrw10\\brdrcf5\\clbrdrl\\brdrs\\brdrw10\\brdrcf5\\clbrdrb\\brdrs\\brdrw10\\brdrcf5\\clbrdrr\\brdrs\\brdrw10\\brdrcf5";
                if (header) out += "\\clcbpat6";
                out += "\\cellx" + std::to_string(left + available * static_cast<int>(i) / static_cast<int>(count));
            }
            out += '\n';
        }
        const int padding = std::clamp(paragraphSpacing / 2, 60, 150);
        out += "{\\pard\\plain\\intbl\\f0\\fs" + std::to_string(fontSize - 1) + "\\cf1\\sa" + std::to_string(padding)
            + "\\sb" + std::to_string(padding) + lineSpacing(std::min(lineHeight, 150));
        if (align == MD_ALIGN_CENTER) out += "\\qc";
        else if (align == MD_ALIGN_RIGHT) out += "\\qr";
        else out += "\\ql";
        if (header) out += "\\b";
        out += ' ';
        paragraph = cell = true;
    }
};

// Catch allocations inside each callback so C parser frames never see a C++
// exception and can free their working buffers on the normal abort path.
template <typename Fn> int guarded(Fn&& fn) noexcept { try { fn(); return 0; } catch (...) { return 1; } }

} // namespace

PreviewResult renderMarkdown(std::string_view utf8, const PreviewOptions& options) {
    try {
        Renderer renderer(options);
        if (utf8.size() > std::numeric_limits<MD_SIZE>::max()) return {"{\\rtf1 Input too large.}", false};
        MD_PARSER parser{};
        parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
        parser.enter_block = [](MD_BLOCKTYPE t, void* d, void* u) { return guarded([&] { static_cast<Renderer*>(u)->enterBlock(t, d); }); };
        parser.leave_block = [](MD_BLOCKTYPE t, void*, void* u) { return guarded([&] { static_cast<Renderer*>(u)->leaveBlock(t); }); };
        parser.enter_span = [](MD_SPANTYPE t, void*, void* u) { return guarded([&] { static_cast<Renderer*>(u)->enterSpan(t); }); };
        parser.leave_span = [](MD_SPANTYPE t, void*, void* u) { return guarded([&] { static_cast<Renderer*>(u)->leaveSpan(t); }); };
        parser.text = [](MD_TEXTTYPE t, const MD_CHAR* v, MD_SIZE n, void* u) { return guarded([&] { static_cast<Renderer*>(u)->text(t, {v, n}); }); };
        const int status = md_parse(utf8.empty() ? "" : utf8.data(), static_cast<MD_SIZE>(utf8.size()), &parser, &renderer);
        if (status != 0) return {"{\\rtf1\\ansi Preview could not be rendered.}", false};
        renderer.out += '}';
        return {std::move(renderer.out), true};
    } catch (...) { return {"{\\rtf1\\ansi Preview could not be rendered.}", false}; }
}

} // namespace md
