#pragma once

#include <string>
#include <string_view>

namespace md {

struct PreviewOptions {
    bool dark = false;
    int widthTwips = 7500;
    std::string fontFamily = "Microsoft YaHei";
    int fontSizeHalfPoints = 26;
    int lineHeightPercent = 165;
    int paragraphSpacingTwips = 180;
    bool reading = false;

    bool operator==(const PreviewOptions& other) const {
        return dark == other.dark && widthTwips == other.widthTwips
            && fontFamily == other.fontFamily && fontSizeHalfPoints == other.fontSizeHalfPoints
            && lineHeightPercent == other.lineHeightPercent
            && paragraphSpacingTwips == other.paragraphSpacingTwips && reading == other.reading;
    }
    bool operator!=(const PreviewOptions& other) const { return !(*this == other); }
};

struct PreviewResult {
    std::string rtf;
    bool ok = true;
};

// Produces an ASCII-only, self-contained RTF document. Links and images never
// create RTF fields, embedded objects, or network requests.
PreviewResult renderMarkdown(std::string_view utf8, const PreviewOptions& options = {});

} // namespace md
