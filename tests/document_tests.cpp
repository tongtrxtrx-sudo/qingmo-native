#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../src/document.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
    std::wstring path;
    std::vector<std::wstring> files;
    TempDirectory() {
        wchar_t root[MAX_PATH + 1]{};
        require(GetTempPathW(MAX_PATH, root) != 0, "GetTempPath failed");
        for (unsigned i = 0; i < 100; ++i) {
            path = std::wstring(root) + L"md-document-tests-" + std::to_wstring(GetCurrentProcessId()) +
                   L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(i);
            if (CreateDirectoryW(path.c_str(), nullptr)) return;
            require(GetLastError() == ERROR_ALREADY_EXISTS, "CreateDirectory failed");
        }
        throw std::runtime_error("Cannot create test directory");
    }
    ~TempDirectory() {
        for (const auto& file : files) {
            SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(file.c_str());
        }
        RemoveDirectoryW(path.c_str());
    }
    std::wstring file(const std::wstring& name) {
        files.push_back(path + L"\\" + name);
        return files.back();
    }
};

void writeRaw(const std::wstring& path, const std::string& bytes) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "fixture open failed");
    DWORD written = 0;
    const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0;
    CloseHandle(handle);
    require(ok && written == bytes.size(), "fixture write failed");
}

std::string readRaw(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "raw read open failed");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart > 1024 * 1024) {
        CloseHandle(handle); throw std::runtime_error("unexpected fixture size");
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != 0;
    CloseHandle(handle);
    require(ok && read == bytes.size(), "raw read failed");
    return bytes;
}

std::string expectedUtf16(bool littleEndian) {
    const unsigned units[] = {0xfeff, '#', ' ', 0x4e2d, 0x6587, ' ', 0xd83d, 0xde42,
                              '\r', '\n', 'a', '\n', 'b', '\r', 'c', '\t', 'z'};
    std::string bytes;
    for (unsigned unit : units) {
        bytes.push_back(static_cast<char>(littleEndian ? unit & 255 : unit >> 8));
        bytes.push_back(static_cast<char>(littleEndian ? unit >> 8 : unit & 255));
    }
    return bytes;
}

void testRoundtrips(TempDirectory& dir) {
    const std::string text = u8"# \u4e2d\u6587 \U0001f642\r\na\nb\rc\tz";
    const md::Encoding encodings[] = {md::Encoding::Utf8, md::Encoding::Utf8Bom,
                                      md::Encoding::Utf16LE, md::Encoding::Utf16BE};
    int index = 0;
    for (const auto encoding : encodings) {
        const auto path = dir.file(L"\u4e2d\u6587-\U0001f642-" + std::to_wstring(index++) + L".md");
        md::FileStamp missing, stamp;
        std::wstring error;
        require(md::saveDocument(path, text, encoding, &missing, stamp, error), "initial save failed");
        require(stamp.exists && md::sameStamp(stamp, md::fileStamp(path)), "saved stamp mismatch");
        const std::string expected = encoding == md::Encoding::Utf8 ? text :
            encoding == md::Encoding::Utf8Bom ? std::string("\xef\xbb\xbf", 3) + text :
            expectedUtf16(encoding == md::Encoding::Utf16LE);
        require(readRaw(path) == expected, "encoded bytes/newlines not preserved");
        md::Document doc;
        require(md::loadDocument(path, doc, error), "roundtrip load failed");
        require(doc.text == text && doc.encoding == encoding, "decoded roundtrip differs");
        require(md::saveDocument(path, doc.text, doc.encoding, &doc.stamp, stamp, error), "replacement save failed");
        require(readRaw(path) == expected, "replacement changed encoding/newlines");
        require(md::sameStamp(stamp, md::fileStamp(path)), "replacement saved stamp mismatch");
        require(md::saveDocument(path, {}, encoding, &stamp, stamp, error), "empty save failed");
        require(md::loadDocument(path, doc, error), "empty load failed");
        require(doc.text.empty() && doc.encoding == encoding, "empty roundtrip differs");
        require(doc.stamp.size == (encoding == md::Encoding::Utf8 ? 0 : encoding == md::Encoding::Utf8Bom ? 3 : 2),
                "empty file BOM differs");
    }
}

void testInvalid(TempDirectory& dir) {
    const auto path = dir.file(L"invalid.md");
    const std::vector<std::string> invalid = {
        std::string("a\0b", 3), std::string("a\x01", 2), std::string("\x7f", 1),
        std::string("\xc0\xaf", 2), std::string("\x80", 1), std::string("\xc2", 1),
        std::string("\xe2\x28\xa1", 3), std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4), std::string("\xf0\x80\x80\x80", 4),
        std::string("\xff\xfe\x61", 3), std::string("\xff\xfe\x00\xd8", 4),
        std::string("\xff\xfe\x00\xdc", 4), std::string("\xff\xfe\x00\xd8\x61\x00", 6),
        std::string("\xfe\xff\xd8\x00\x00\x61", 6), std::string("\xfe\xff\x00\x00", 4),
        std::string("\xff\xfe\x00\x00", 4), std::string("\xef\xbb\xbf\xff", 4)
    };
    for (const auto& bytes : invalid) {
        writeRaw(path, bytes);
        md::Document doc{"keep previous", md::Encoding::Utf16BE, {true, 22, 33}};
        std::wstring error;
        require(!md::loadDocument(path, doc, error), "invalid input was accepted");
        require(!error.empty(), "invalid input has no diagnostic");
        require(doc.text == "keep previous" && doc.encoding == md::Encoding::Utf16BE &&
                doc.stamp.size == 22 && doc.stamp.modified == 33, "failed load mutated document");
    }
    md::Document previous{"keep", md::Encoding::Utf8, {}};
    std::wstring error;
    require(!md::loadDocument(dir.path + L"\\missing.md", previous, error), "missing file loaded");
    require(previous.text == "keep", "missing file changed document");
    require(!md::loadDocument(dir.path, previous, error), "directory loaded");
    require(!md::loadDocument({}, previous, error), "empty path loaded");
    const std::wstring embeddedNul = path + std::wstring(L"\0ignored", 8);
    require(!md::loadDocument(embeddedNul, previous, error), "embedded NUL path accepted");
}

void testConflictsAndFailures(TempDirectory& dir) {
    const auto path = dir.file(L"conflict.md");
    writeRaw(path, "original");
    md::Document doc;
    std::wstring error;
    require(md::loadDocument(path, doc, error), "load for conflict failed");
    writeRaw(path, "externally changed");
    md::FileStamp output{true, 123, 456};
    require(!md::saveDocument(path, "mine", md::Encoding::Utf8, &doc.stamp, output, error), "external mutation overwritten");
    require(readRaw(path) == "externally changed", "conflict changed original");
    require(output.size == 123 && output.modified == 456, "failed save changed output stamp");
    md::FileStamp missing;
    require(!md::saveDocument(path, "mine", md::Encoding::Utf8, &missing, output, error), "create-only clobbered existing file");
    require(readRaw(path) == "externally changed", "create-only changed original");
    const auto current = md::fileStamp(path);
    require(!md::saveDocument(path, std::string("x\0y", 3), md::Encoding::Utf8, &current, output, error), "binary save accepted");
    require(readRaw(path) == "externally changed", "invalid save changed original");
    require(!md::saveDocument(path, "mine", static_cast<md::Encoding>(99), &current, output, error), "invalid encoding accepted");
    require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "set readonly failed");
    require(!md::saveDocument(path, "mine", md::Encoding::Utf8, &current, output, error), "readonly save succeeded");
    require(readRaw(path) == "externally changed", "readonly save changed original");
    require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != 0, "clear readonly failed");

    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "cannot lock test file");
    const bool lockSave = md::saveDocument(path, "mine", md::Encoding::Utf8, &current, output, error);
    CloseHandle(locked);
    require(!lockSave, "save to locked target succeeded");
    require(readRaw(path) == "externally changed", "locked failure damaged original");

    require(DeleteFileW(path.c_str()) != 0, "delete fixture failed");
    require(!md::saveDocument(path, "mine", md::Encoding::Utf8, &current, output, error), "external deletion ignored");
    require(!md::fileStamp(path).exists, "failed save recreated externally deleted file");
    require(!md::saveDocument(dir.path + L"\\absent\\file.md", "mine", md::Encoding::Utf8, nullptr, output, error),
            "missing parent save succeeded");
    require(!md::saveDocument(dir.path, "mine", md::Encoding::Utf8, nullptr, output, error), "save over directory succeeded");
}

void testSizeLimit(TempDirectory& dir) {
    const auto path = dir.file(L"oversized.md");
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "large fixture create failed");
    LARGE_INTEGER size;
    size.QuadPart = md::maxDocumentBytes + 1;
    const bool ok = SetFilePointerEx(handle, size, nullptr, FILE_BEGIN) && SetEndOfFile(handle);
    CloseHandle(handle);
    require(ok, "large fixture size failed");
    md::Document doc{"unchanged", md::Encoding::Utf8, {}};
    std::wstring error;
    require(!md::loadDocument(path, doc, error), "oversized file accepted");
    require(doc.text == "unchanged" && !error.empty(), "oversized failure lost original/diagnostic");
}

void testNoTemporaryLeaks(const TempDirectory& dir) {
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((dir.path + L"\\.md-save-*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        FindClose(search);
        throw std::runtime_error("temporary/backup files leaked");
    }
    require(GetLastError() == ERROR_FILE_NOT_FOUND, "temporary cleanup inspection failed");
}

} // namespace

int main() {
    try {
        TempDirectory dir;
        testRoundtrips(dir);
        testInvalid(dir);
        testConflictsAndFailures(dir);
        testSizeLimit(dir);
        testNoTemporaryLeaks(dir);
        std::puts("document_tests: all tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "document_tests: FAILED: %s\n", error.what());
        return 1;
    }
}
