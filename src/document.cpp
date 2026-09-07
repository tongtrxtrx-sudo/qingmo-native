#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "document.hpp"

#include <algorithm>
#include <atomic>
#include <new>
#include <utility>

namespace md {
namespace {

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value != INVALID_HANDLE_VALUE; }
    void close() { if (valid()) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
};

struct TempFile {
    std::wstring path;
    Handle handle;
    ~TempFile() { handle.close(); if (!path.empty()) DeleteFileW(path.c_str()); }
};

std::wstring winError(const wchar_t* action, DWORD code = GetLastError()) {
    wchar_t message[512]{};
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, message, 512, nullptr);
    std::wstring result(action);
    result += L" (" + std::to_wstring(code) + L")";
    if (count) { result += L": "; result.append(message, count); }
    return result;
}

std::uint64_t timeValue(const FILETIME& time) {
    return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool validPath(const std::wstring& path, std::wstring& error) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos) {
        error = L"文件路径为空或包含无效字符。";
        return false;
    }
    return true;
}

bool absolutePath(const std::wstring& path, std::wstring& resolved, std::wstring& error) {
    if (!validPath(path, error)) return false;
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!needed) { error = winError(L"无法解析文件路径"); return false; }
    resolved.resize(needed);
    const DWORD length = GetFullPathNameW(path.c_str(), needed, resolved.data(), nullptr);
    if (!length || length >= needed) { error = L"无法解析文件路径，当前目录可能发生变化。"; return false; }
    resolved.resize(length);
    return true;
}

bool queryStamp(const std::wstring& path, FileStamp& stamp, std::wstring& error,
                DWORD* attributes = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            stamp = {};
            if (attributes) *attributes = 0;
            return true;
        }
        error = winError(L"无法读取文件状态", code);
        return false;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        error = L"所选路径是文件夹。";
        return false;
    }
    stamp = {true, (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
             timeValue(data.ftLastWriteTime)};
    if (attributes) *attributes = data.dwFileAttributes;
    return true;
}

bool handleStamp(HANDLE handle, FileStamp& stamp, std::wstring& error) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        error = winError(L"无法读取文件状态");
        return false;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        error = L"所选路径是文件夹。";
        return false;
    }
    stamp = {true, (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
             timeValue(info.ftLastWriteTime)};
    return true;
}

bool textCodepoint(std::uint32_t cp, std::wstring& error) {
    if ((cp < 32 && cp != 9 && cp != 10 && cp != 13) || cp == 127) {
        error = L"文件包含 NUL 或二进制控制字符，无法作为 Markdown 文本打开或保存。";
        return false;
    }
    return true;
}

bool nextUtf8(std::string_view text, std::size_t& pos, std::uint32_t& cp,
              std::wstring& error) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char first = byte(pos++);
    if (first < 0x80) { cp = first; return textCodepoint(cp, error); }
    int trailing;
    std::uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) { trailing = 1; cp = first & 31; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { trailing = 2; cp = first & 15; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { trailing = 3; cp = first & 7; minimum = 0x10000; }
    else { error = L"文件不是有效的 UTF-8；仅支持 UTF-8 或带 BOM 的 UTF-16。"; return false; }
    if (text.size() - pos < static_cast<std::size_t>(trailing)) {
        error = L"UTF-8 字符不完整。"; return false;
    }
    for (int i = 0; i < trailing; ++i) {
        const auto next = byte(pos++);
        if ((next & 0xc0) != 0x80) { error = L"UTF-8 字节序列无效。"; return false; }
        cp = (cp << 6) | (next & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        error = L"UTF-8 字符编码无效。"; return false;
    }
    return textCodepoint(cp, error);
}

bool validateUtf8(std::string_view text, std::wstring& error) {
    std::size_t pos = 0;
    std::uint32_t cp;
    while (pos < text.size()) if (!nextUtf8(text, pos, cp, error)) return false;
    return true;
}

bool appendUtf8(std::string& text, std::uint32_t cp, std::wstring& error) {
    const std::size_t count = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (text.size() > maxDocumentBytes - count) {
        error = L"转换后的文本超过 128 MiB 限制。"; return false;
    }
    if (count == 1) text.push_back(static_cast<char>(cp));
    else {
        if (count == 2) text.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        else {
            if (count == 3) text.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            else {
                text.push_back(static_cast<char>(0xf0 | (cp >> 18)));
                text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
            }
            text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        }
        text.push_back(static_cast<char>(0x80 | (cp & 63)));
    }
    return true;
}

bool decodeUtf16(std::string_view bytes, bool littleEndian, std::string& text,
                 std::wstring& error) {
    if (bytes.size() % 2) { error = L"UTF-16 文件末尾缺少字节。"; return false; }
    text.reserve(bytes.size());
    auto unit = [&](std::size_t pos) -> std::uint32_t {
        const auto a = static_cast<unsigned char>(bytes[pos]);
        const auto b = static_cast<unsigned char>(bytes[pos + 1]);
        return littleEndian ? a | (std::uint32_t(b) << 8) : (std::uint32_t(a) << 8) | b;
    };
    for (std::size_t pos = 0; pos < bytes.size(); pos += 2) {
        std::uint32_t cp = unit(pos);
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (pos + 3 >= bytes.size()) { error = L"UTF-16 代理对不完整。"; return false; }
            const auto low = unit(pos + 2);
            if (low < 0xdc00 || low > 0xdfff) { error = L"UTF-16 代理对无效。"; return false; }
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            pos += 2;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            error = L"UTF-16 包含孤立的低代理字符。"; return false;
        }
        if (!textCodepoint(cp, error) || !appendUtf8(text, cp, error)) return false;
    }
    return true;
}

bool writeBytes(HANDLE handle, std::string_view bytes, std::wstring& error) {
    while (!bytes.empty()) {
        const DWORD amount = static_cast<DWORD>((std::min)(bytes.size(), std::size_t(1024 * 1024)));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), amount, &written, nullptr) || written == 0) {
            error = winError(L"写入文件失败"); return false;
        }
        bytes.remove_prefix(written);
    }
    return true;
}

bool writeText(HANDLE handle, std::string_view text, Encoding encoding, std::wstring& error) {
    if (encoding == Encoding::Utf8) return writeBytes(handle, text, error);
    if (encoding == Encoding::Utf8Bom) {
        return writeBytes(handle, std::string_view("\xef\xbb\xbf", 3), error) && writeBytes(handle, text, error);
    }
    const bool littleEndian = encoding == Encoding::Utf16LE;
    if (!writeBytes(handle, littleEndian ? std::string_view("\xff\xfe", 2) : std::string_view("\xfe\xff", 2), error)) return false;
    std::string buffer;
    buffer.reserve(65536);
    std::size_t pos = 0, bytesWritten = 2;
    auto appendUnit = [&](std::uint32_t unit) {
        buffer.push_back(static_cast<char>(littleEndian ? unit & 255 : unit >> 8));
        buffer.push_back(static_cast<char>(littleEndian ? unit >> 8 : unit & 255));
    };
    while (pos < text.size()) {
        std::uint32_t cp;
        if (!nextUtf8(text, pos, cp, error)) return false;
        if (cp < 0x10000) appendUnit(cp);
        else { cp -= 0x10000; appendUnit(0xd800 | (cp >> 10)); appendUnit(0xdc00 | (cp & 1023)); }
        if (bytesWritten + buffer.size() > maxDocumentBytes) {
            error = L"编码后的文件超过 128 MiB 限制。"; return false;
        }
        if (buffer.size() >= 65532) {
            if (!writeBytes(handle, buffer, error)) return false;
            bytesWritten += buffer.size();
            buffer.clear();
        }
    }
    return writeBytes(handle, buffer, error);
}

bool createTemp(const std::wstring& target, TempFile& temp, std::wstring& error) {
    static std::atomic<unsigned long long> sequence{0};
    const auto slash = target.find_last_of(L"\\/");
    const auto directory = slash == std::wstring::npos ? L"" : target.substr(0, slash + 1);
    for (int attempt = 0; attempt < 64; ++attempt) {
        auto candidate = directory + L".md-save-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                         std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1)) + L".tmp";
        HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            temp.path = std::move(candidate); temp.handle.value = handle; return true;
        }
        const auto code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
            error = winError(L"无法在文件所在目录创建临时文件", code); return false;
        }
    }
    error = L"无法创建唯一的临时文件。"; return false;
}

} // namespace

bool sameStamp(const FileStamp& a, const FileStamp& b) {
    return a.exists == b.exists && (!a.exists || (a.size == b.size && a.modified == b.modified));
}

FileStamp fileStamp(const std::wstring& path) {
    FileStamp result;
    std::wstring error;
    if (validPath(path, error)) queryStamp(path, result, error);
    return result;
}

bool loadDocument(const std::wstring& path, Document& result, std::wstring& error) {
    error.clear();
    try {
        std::wstring resolved;
        if (!absolutePath(path, resolved, error)) return false;
        Handle handle(CreateFileW(resolved.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!handle.valid()) { error = winError(L"打开文件失败"); return false; }
        Document candidate;
        if (!handleStamp(handle.value, candidate.stamp, error)) return false;
        if (candidate.stamp.size > maxDocumentBytes) { error = L"文件超过 128 MiB 限制。"; return false; }
        candidate.text.resize(static_cast<std::size_t>(candidate.stamp.size));
        std::size_t offset = 0;
        while (offset < candidate.text.size()) {
            DWORD read = 0;
            const DWORD amount = static_cast<DWORD>((std::min)(candidate.text.size() - offset, std::size_t(1024 * 1024)));
            if (!ReadFile(handle.value, candidate.text.data() + offset, amount, &read, nullptr)) {
                error = winError(L"读取文件失败"); return false;
            }
            if (read == 0) { error = L"读取期间文件发生了变化，请重新打开。"; return false; }
            offset += read;
        }
        FileStamp after, current;
        if (!handleStamp(handle.value, after, error) || !queryStamp(resolved, current, error)) return false;
        if (!sameStamp(candidate.stamp, after) || !sameStamp(after, current)) {
            error = L"读取期间文件发生了变化，请重新打开。"; return false;
        }
        std::string_view bytes(candidate.text);
        if (bytes.size() >= 2 && (bytes.substr(0, 2) == std::string_view("\xff\xfe", 2) ||
                                  bytes.substr(0, 2) == std::string_view("\xfe\xff", 2))) {
            const bool littleEndian = static_cast<unsigned char>(bytes[0]) == 0xff;
            candidate.encoding = littleEndian ? Encoding::Utf16LE : Encoding::Utf16BE;
            std::string decoded;
            if (!decodeUtf16(bytes.substr(2), littleEndian, decoded, error)) return false;
            candidate.text = std::move(decoded);
        } else {
            if (bytes.size() >= 3 && bytes.substr(0, 3) == std::string_view("\xef\xbb\xbf", 3)) {
                candidate.encoding = Encoding::Utf8Bom;
                candidate.text.erase(0, 3);
            }
            if (!validateUtf8(candidate.text, error)) return false;
        }
        result = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) { error = L"内存不足，无法打开文件。"; return false; }
}

bool saveDocument(const std::wstring& path, std::string_view utf8, Encoding encoding,
                  const FileStamp* expected, FileStamp& savedStamp, std::wstring& error) {
    error.clear();
    try {
        std::wstring resolved;
        if (!absolutePath(path, resolved, error)) return false;
        if (encoding != Encoding::Utf8 && encoding != Encoding::Utf8Bom &&
            encoding != Encoding::Utf16LE && encoding != Encoding::Utf16BE) {
            error = L"不支持的文本编码。"; return false;
        }
        if (utf8.size() > maxDocumentBytes || (encoding == Encoding::Utf8Bom && utf8.size() > maxDocumentBytes - 3)) {
            error = L"文本或编码后的文件超过 128 MiB 限制。"; return false;
        }
        if (!validateUtf8(utf8, error)) return false;
        FileStamp initial;
        DWORD attributes = 0;
        if (!queryStamp(resolved, initial, error, &attributes)) return false;
        if (expected && !sameStamp(initial, *expected)) {
            error = L"文件已在外部修改、删除或创建；为避免覆盖，请重新打开或另存为。"; return false;
        }
        if (attributes & FILE_ATTRIBUTE_READONLY) { error = L"文件为只读，无法保存。"; return false; }
        TempFile temp, backup;
        if (!createTemp(resolved, temp, error) || !writeText(temp.handle.value, utf8, encoding, error)) return false;
        if (!FlushFileBuffers(temp.handle.value)) { error = winError(L"刷新文件到磁盘失败"); return false; }
        temp.handle.close();
        FileStamp written;
        if (!queryStamp(temp.path, written, error)) return false;
        // Reserve our own backup path. ReplaceFileW replaces this empty placeholder.
        // A backup protects the original even in its documented partial-failure cases.
        if (initial.exists) {
            if (!createTemp(resolved, backup, error)) return false;
            backup.handle.close();
        }
        FileStamp current;
        if (!queryStamp(resolved, current, error, &attributes)) return false;
        if (!sameStamp(initial, current)) {
            error = L"保存期间文件发生了变化；为避免覆盖，已取消保存。"; return false;
        }
        if (attributes & FILE_ATTRIBUTE_READONLY) { error = L"文件为只读，无法保存。"; return false; }
        const BOOL committed = initial.exists
            ? ReplaceFileW(resolved.c_str(), temp.path.c_str(), backup.path.c_str(), 0, nullptr, nullptr)
            : MoveFileExW(temp.path.c_str(), resolved.c_str(), MOVEFILE_WRITE_THROUGH);
        if (!committed) {
            const DWORD code = GetLastError();
            error = winError(L"保存失败", code);
            if (initial.exists && code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                // Never overwrite a file another process may have created meanwhile.
                if (!MoveFileExW(backup.path.c_str(), resolved.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    error += L"\n原文件已保留在备份：" + backup.path;
                    backup.path.clear();
                }
            }
            return false;
        }
        temp.path.clear();
        savedStamp = written;
        return true;
    } catch (const std::bad_alloc&) { error = L"内存不足，无法保存文件。"; return false; }
}

} // namespace md
