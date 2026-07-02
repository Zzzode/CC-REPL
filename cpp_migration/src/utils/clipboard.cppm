// @file clipboard.cppm
// @brief System clipboard image detection + read (macOS).  Faithful to TS
// src/utils/imagePaste.ts hasImageInClipboard / getImageFromClipboard, minus
// the native NSPasteboard fast-path (we use the osascript fallback, which TS
// also falls through to).  Linux/Windows return false/nullopt (TS parity).
module;

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>        // open, O_RDWR
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/wait.h>     // waitpid
#include <system_error>
#include <unistd.h>       // fork, setsid, dup2, execl, _exit, STDIN_FILENO
#include <vector>

export module cc.utils.clipboard;

export namespace cc::utils::clipboard {

// ── osascript invocation gotcha (macOS) ─────────────────────────────────
// cc-repl runs the terminal in raw mode (FTXUI termios: ICANON/ECHO off).
// std::system() forks a child that INHERITS fd 0 = the raw-mode terminal.
// osascript, on detecting a TTY on stdin, takes a code path that misbehaves
// under raw mode and exits non-zero — so the SAME osascript command that works
// in a normal shell fails silently inside the app (read_image_png() returned
// nullopt, [Image #N] placeholder erased ~700ms later). User-reported
// 2026-07-01 (strike 4).
//
// Fix: run osascript detached — fork()+setsid()+exec() with stdin/stdout/stderr
// all pointed at /dev/null and every other inherited fd closed. setsid() puts
// the child in a new session with NO controlling terminal, so osascript can't
// see the raw-mode TTY at all. We can't get this through std::system()/sh
// alone (`< /dev/null` only redirects the grandchild's fd 0, and sh doesn't
// setsid), so we do it manually in run_detached().
//
// NOTE: a SECOND user report ("Ctrl+V pressed 8×, only 4 register", strike 5)
// turned out to be UNRELATED to osascript — it was the macOS line discipline's
// VLNEXT (Ctrl+V literal-next) still active in non-canonical mode, fixed in
// app.cppm RunApp by clearing c_cc[VLNEXT]. run_detached() here is kept as
// good hygiene (fully isolates the osascript subprocess) but is NOT what fixes
// keystroke loss.

/// Run `cmd` via /bin/sh -c in a new session with no controlling terminal and
/// all stdio redirected to /dev/null.  Returns the raw waitpid() status (0 on
/// clean exit), or -1 on fork/exec failure.  Blocks the caller until the child
/// finishes — callers that need non-blocking behaviour should run this on a
/// worker thread (as SpawnPasteWorker does).
inline int run_detached(const std::string& cmd) noexcept {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Child: new session, no controlling terminal. osascript can no longer
        // see cc-repl's raw-mode TTY, so it behaves as in batch mode.
        (void)setsid();
        // Redirect 0/1/2 to /dev/null (osascript writes its PNG via AppleScript
        // file I/O, not via stdout, so this is safe).
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) (void)close(devnull);
        }
        // Close every other inherited fd so the osascript chain doesn't keep a
        // dup of the terminal or any other cc-repl fd.
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd <= 0) maxfd = 256;
        for (int fdnum = 3; fdnum < maxfd; ++fdnum) {
            (void)close(fdnum);
        }
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);  // exec failed
    }
    // Parent: wait for the child, retrying on EINTR.
    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) return -1;
    }
    return status;
}

/// True if the system clipboard currently holds an image.
/// macOS: `osascript -e 'the clipboard as «class PNGf»'` — exit 0 ⇔ an image
/// is present (matches TS hasImageInClipboard's osascript fallback).  Other
/// platforms: false (TS returns false off-darwin).
[[nodiscard]] inline bool has_image() noexcept {
#if defined(__APPLE__)
    // Only the exit code matters; non-zero ⇔ no image (osascript errors with
    // "Can't make clipboard into type alias"). run_detached() isolates the
    // subprocess from cc-repl's raw-mode terminal — see the block above.
    return run_detached("osascript -e 'the clipboard as «class PNGf»'") == 0;
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
    //
    // IMPORTANT (1): «class PNGf» is typed as literal UTF-8 characters in the
    // C++ source — the same byte-sequence approach has_image() uses and TS
    // saveImage's AppleScript uses.  DO NOT replace with `\xc2\xab` escapes:
    // double-backslash them and the shell sees literal ASCII backslashes;
    // single-backslash and the preprocessor decodes to bytes but the
    // concatenation chain is clearer as a UTF-8 source literal.
    //
    // IMPORTANT (2): run_detached() (NOT std::system) is mandatory — see the
    // gotcha block above. setsid()+/dev/null isolates osascript from cc-repl's
    // raw-mode terminal so it doesn't see a TTY on stdin and fail.
    std::string script =
        "osascript "
        "-e 'set png_data to (the clipboard as «class PNGf»)' "
        "-e 'set fp to open for access POSIX file \"" + tmp_s +
        "\" with write permission' "
        "-e 'set eof of fp to 0' "
        "-e 'write png_data to fp' "
        "-e 'close access fp'";
    if (run_detached(script) != 0) {
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
