#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace md {

enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE };

struct FileStamp {
    bool exists = false;
    std::uint64_t size = 0;
    std::uint64_t modified = 0;
};

struct Document {
    std::string text;
    Encoding encoding = Encoding::Utf8;
    FileStamp stamp;
};

// Applies both to on-disk bytes and decoded UTF-8 bytes.
inline constexpr std::size_t maxDocumentBytes = 128 * 1024 * 1024;

// Failures leave result/savedStamp unchanged. Text remains UTF-8 in memory.
bool loadDocument(const std::wstring& path, Document& result, std::wstring& error);
// expected == nullptr accepts the initial state of the destination. A non-null
// stamp also detects changes since loading; {false, 0, 0} means create only.
// Stamp comparisons are best-effort, not an atomic compare-and-swap against
// arbitrary external writers. Successful saves preserve the supplied newlines.
bool saveDocument(const std::wstring& path, std::string_view utf8, Encoding encoding,
                  const FileStamp* expected, FileStamp& savedStamp, std::wstring& error);
// Returns a missing stamp on inaccessible/invalid paths as well as missing files.
// loadDocument/saveDocument distinguish these cases and report errors.
FileStamp fileStamp(const std::wstring& path);
bool sameStamp(const FileStamp& a, const FileStamp& b);

} // namespace md
