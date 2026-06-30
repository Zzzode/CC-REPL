// @file clipboard.cppm
// @brief System clipboard image detection + read (macOS).  Faithful to TS
// src/utils/imagePaste.ts hasImageInClipboard / getImageFromClipboard, minus
// the native NSPasteboard fast-path (we use the osascript fallback, which TS
// also falls through to).  Linux/Windows return false/nullopt (TS parity).
module;

#include <cstdint>
#include <cstdio>      // std::system
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

export module cc.utils.clipboard;

export namespace cc::utils::clipboard {

/// True if the system clipboard currently holds an image.
/// macOS: `osascript -e 'the clipboard as «class PNGf»'` — exit 0 ⇔ an image
/// is present (matches TS hasImageInClipboard's osascript fallback).  Other
/// platforms: false (TS returns false off-darwin).
[[nodiscard]] inline bool has_image() noexcept {
#if defined(__APPLE__)
    // Redirect all output; only the exit code matters.  non-zero ⇔ no image
    // (osascript errors with "Can't make clipboard into type alias" etc.).
    return std::system("osascript -e 'the clipboard as «class PNGf»' >/dev/null 2>&1") == 0;
#else
    return false;
#endif
}

/// Read the clipboard image as PNG bytes (macOS).  Returns nullopt if there is
/// no image or the read fails.  Writes the clipboard PNG to a temp file via
/// osascript (mirrors TS saveImage), then reads the bytes back.  Always returns
/// nullopt off-macOS.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> read_image_png() {
#if defined(__APPLE__)
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "cc-repl-clipboard.png";
    const std::string tmp_s = tmp.string();
    // AppleScript: write the clipboard's PNG data to the temp file.  `set eof
    // to 0` truncates first so a stale larger file can't leave trailing bytes.
    std::string script =
        "osascript "
        "-e 'set png_data to (the clipboard as \\xc2\\xabclass PNGf\\xc2\\xbb)' "
        "-e 'set fp to open for access POSIX file \"" + tmp_s +
        "\" with write permission' "
        "-e 'set eof of fp to 0' "
        "-e 'write png_data to fp' "
        "-e 'close access fp' >/dev/null 2>&1";
    if (std::system(script.c_str()) != 0) {
        std::error_code rc; fs::remove(tmp, rc);
        return std::nullopt;
    }
    std::ifstream f(tmp, std::ios::binary);
    if (!f) {
        std::error_code rc; fs::remove(tmp, rc);
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    std::error_code rc; fs::remove(tmp, rc);
    if (bytes.empty()) return std::nullopt;
    return bytes;
#else
    return std::nullopt;
#endif
}

}  // namespace cc::utils::clipboard
