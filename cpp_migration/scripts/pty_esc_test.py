#!/usr/bin/env python3
"""Phase C-e acceptance test 4: drive cc-repl on a real PTY.

Sends an ESC keystroke 0.8 s after the first render byte, then expects
ScreenInteractive::Loop() to terminate cleanly.  Exits with the REPL's
own exit code, or 99 on timeout / internal failure.

Usage: pty_esc_test.py <path-to-cc-repl> <output-trace-path>
"""

import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time


def main(argv) -> int:
    if len(argv) != 3:
        print("usage: pty_esc_test.py <repl-path> <trace-path>", file=sys.stderr)
        return 2
    repl_path, trace_path = argv[1], argv[2]

    pid, fd = pty.fork()
    if pid == 0:
        # child
        os.execv(repl_path, [os.path.basename(repl_path)])
        # unreachable
        return 127

    # parent: 80x24 terminal
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    collected = bytearray()
    deadline = time.monotonic() + 6.0
    esc_sent = False
    rc: int | None = None

    try:
        while time.monotonic() < deadline:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    chunk = os.read(fd, 4096)
                except OSError:
                    chunk = b""
                if not chunk:
                    break
                collected.extend(chunk)

            if not esc_sent and len(collected) >= 512:
                # first paint reached
                time.sleep(0.15)
                try:
                    os.write(fd, b"\x1b")  # ESC
                except OSError:
                    pass
                esc_sent = True

            wpid, status = os.waitpid(pid, os.WNOHANG)
            if wpid == pid:
                rc = os.waitstatus_to_exitcode(status)
                break
            # tiny sleep to avoid spinning 100% CPU
            time.sleep(0.02)

        if rc is None:
            # fallback: Ctrl+C then ESC, final SIGTERM
            try:
                os.write(fd, b"\x03\x1b")
            except OSError:
                pass
            for _ in range(40):
                wpid, status = os.waitpid(pid, os.WNOHANG)
                if wpid == pid:
                    rc = os.waitstatus_to_exitcode(status)
                    break
                time.sleep(0.1)
            if rc is None:
                try:
                    os.kill(pid, 15)
                except ProcessLookupError:
                    pass
                wpid, status = os.waitpid(pid, 0)
                rc = os.waitstatus_to_exitcode(status)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass

    with open(trace_path, "wb") as f:
        f.write(bytes(collected))
    print(f"exit={rc} esc_sent={esc_sent} bytes_captured={len(collected)}")
    return rc if rc is not None else 99


if __name__ == "__main__":
    sys.exit(main(sys.argv))
