/// @file tools_smoke.cpp
/// @brief Lightweight standalone smoke test for impl_bash + impl_files.
///
/// Not a ctest — directly linkable executable driven by the migration
/// pipeline's Phase 3-B/C validation checklist.  Exit non-zero on any
/// assertion failure.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <iostream>
#include <vector>
#include <fstream>
#include <filesystem>

import cc.tools.bash.impl;
import cc.tools.files.impl;

using namespace cc::tools::bash::impl;
using namespace cc::tools::files::impl;

static int g_failed = 0;
#define CHECK(expr, ...) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
        std::fprintf(stderr, __VA_ARGS__); \
        std::fprintf(stderr, "\n"); \
        ++g_failed; \
    } else { \
        std::printf("  OK "); \
        std::printf(__VA_ARGS__); \
        std::printf("\n"); \
    } \
} while (0)

static void show(const std::string& s) {
    for (char c : s) {
        if (c == '\n') std::putchar('\\');
        if (c == '\n') std::putchar('n');
        else           std::putchar(static_cast<unsigned char>(c));
    }
}

int main() {
    // ------------------------------------------------------------------
    // Bash smoke
    // ------------------------------------------------------------------
    std::puts("=== Bash ===");
    {
        auto r = RunBash("echo hello");
        std::printf("Smoke Bash(echo hello): combined='");
        show(r.combined);
        std::printf("' exit=%d timed_out=%d allow_fail_err='%s'\n",
                    r.exit_code, r.timed_out ? 1 : 0, r.error_reason.c_str());
        CHECK(r.exit_code == 0, "echo exit=0 got %d", r.exit_code);
        CHECK(r.combined == "hello\n", "echo combined='hello\\n' got len=%zu",
              r.combined.size());
    }
    {
        auto r = RunBash("false");
        CHECK(r.exit_code != 0, "false exits nonzero got %d", r.exit_code);
        CHECK(!r.error_reason.empty(), "non-zero exit produces error_reason");
    }
    {
        BashOptions o;
        o.command         = "sleep 10";
        o.timeout_sec     = 1;
        o.allow_fail      = true;
        auto r = ExecuteBash(std::move(o));
        CHECK(r.timed_out, "sleep 10 with 1s timeout timed_out=true");
        CHECK(r.exit_code != 0, "timeout exit != 0 got %d", r.exit_code);
    }
    {
        BashOptions o;
        o.command         = "echo A; echo B 1>&2; echo C";
        o.combine_streams = false;
        o.allow_fail      = true;
        auto r = ExecuteBash(std::move(o));
        CHECK(r.stdout_only == "A\nC\n", "stdout only correct got='%s'", r.stdout_only.c_str());
        CHECK(r.stderr_only == "B\n",     "stderr only correct got='%s'", r.stderr_only.c_str());
    }
    {
        BashOptions o;
        o.command = "echo $P3_CUSTOM | rev";
        o.env.emplace_back("P3_CUSTOM", "dlrow olleh");
        auto r = ExecuteBash(std::move(o));
        CHECK(r.combined == "hello world\n", "custom env propagated got='%s'", r.combined.c_str());
    }
    {
        // BuildSandboxedInvocation should not explode (we don't actually
        // run sandbox-exec in smoke because tests may run in unprivileged
        // containers that reject the profile).
        auto inv = BuildSandboxedInvocation("ls -la /tmp");
        CHECK(inv.find("sandbox-exec") != std::string::npos,
              "sandboxed invocation contains sandbox-exec");
        CHECK(inv.find("ls -la /tmp") != std::string::npos,
              "sandboxed invocation contains the user command");
    }

    // ------------------------------------------------------------------
    // Files smoke
    // ------------------------------------------------------------------
    std::puts("=== Files ===");
    const std::string path = "/tmp/phase3_f.txt";
    std::remove(path.c_str());

    WriteOptions wo;
    wo.path    = path;
    wo.content = "A=1\nB=2\nC=3\n";
    auto wr = WriteFile(wo);
    CHECK(wr.ok && wr.bytes_written == wo.content.size() && wr.created_new,
          "WriteFile new file bytes=%zu created=%d", size_t(wr.bytes_written), wr.created_new);

    ReadOptions ro; ro.path = path;
    auto rr = ReadFile(ro);
    CHECK(rr.ok && rr.content == "A=1\nB=2\nC=3\n", "ReadFile roundtrip");
    CHECK(rr.lines_read == 3, "lines_read=3 got=%d", rr.lines_read);

    // Slice by offset_line / limit_lines
    ReadOptions ro2; ro2.path = path; ro2.offset_line = 2; ro2.limit_lines = 1;
    auto rr2 = ReadFile(ro2);
    CHECK(rr2.ok && rr2.content == "B=2\n", "slice [2:2] got='%s'", rr2.content.c_str());

    // Edit
    EditOptions eo;
    eo.path       = path;
    eo.old_string = "A=1";
    eo.new_string = "A=42";
    auto er = EditFile(eo);
    CHECK(er.ok && er.replacements == 1, "Edit single match replace ok rpl=%d", er.replacements);
    auto rr3 = ReadFile(ro);
    CHECK(rr3.ok && rr3.content == "A=42\nB=2\nC=3\n",
          "Edit content matches got='%s'", rr3.content.c_str());

    // Edit dry_run
    EditOptions eod;
    eod.path       = path;
    eod.old_string = "B=2";
    eod.new_string = "B=99";
    eod.dry_run    = true;
    auto erd = EditFile(eod);
    CHECK(erd.ok && erd.preview == "A=42\nB=99\nC=3\n", "Edit dry_run previews correctly");
    auto rr4 = ReadFile(ro);
    CHECK(rr4.ok && rr4.content == "A=42\nB=2\nC=3\n",
          "Edit dry_run does not mutate file");

    // Edit: no match → error
    EditOptions eofail;
    eofail.path       = path;
    eofail.old_string = "DOES_NOT_EXIST";
    eofail.new_string = "x";
    auto ef = EditFile(eofail);
    CHECK(!ef.ok && !ef.error.empty(), "Edit missing old_string reports error: %s", ef.error.c_str());

    // Edit: replace_all=true
    EditOptions eora;
    eora.path        = path;
    eora.old_string  = "=";
    eora.new_string  = ":=";
    eora.replace_all = true;
    auto era = EditFile(eora);
    CHECK(era.ok && era.replacements == 3, "Edit replace_all rpl=3 got=%d", era.replacements);
    auto rr5 = ReadFile(ro);
    CHECK(rr5.ok && rr5.content == "A:=42\nB:=2\nC:=3\n",
          "replace_all content matches got='%s'", rr5.content.c_str());

    // ExclusiveNew should fail on existing
    WriteOptions wo2;
    wo2.path    = path;
    wo2.content = "boom";
    wo2.mode    = WriteMode::ExclusiveNew;
    auto wr2 = WriteFile(wo2);
    CHECK(!wr2.ok, "ExclusiveNew on existing file fails");

    // Append mode
    WriteOptions wo3;
    wo3.path    = path;
    wo3.content = "D:=8\n";
    wo3.mode    = WriteMode::Append;
    auto wr3 = WriteFile(wo3);
    CHECK(wr3.ok, "Append write ok");
    auto rr6 = ReadFile(ro);
    CHECK(rr6.ok && rr6.content == "A:=42\nB:=2\nC:=3\nD:=8\n",
          "Append content matches");

    // Binary gate
    {
        const std::string bpath = "/tmp/phase3_bin.bin";
        std::string binary(256, ' ');
        binary[42] = '\0';
        std::ofstream of(bpath, std::ios::binary); of.write(binary.data(), 256); of.close();
        ReadOptions rbo; rbo.path = bpath;
        auto rbr = ReadFile(rbo);
        CHECK(!rbr.ok, "binary gate rejects file with NUL bytes");
        rbo.binary = true;
        auto rbr2 = ReadFile(rbo);
        CHECK(rbr2.ok && rbr2.bytes_read == 256,
              "binary=true bypasses gate bytes=%zu", rbr2.bytes_read);
        std::remove(bpath.c_str());
    }

    std::remove(path.c_str());

    // ------------------------------------------------------------------
    // Glob + Grep
    // ------------------------------------------------------------------
    std::puts("=== Glob + Grep ===");
    {
        // Create a mini tree
        const std::string base = "/tmp/phase3_glob_root";
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(base + "/sub", ec);
        auto touch = [](const std::string& p, std::string_view c) {
            std::ofstream o(p); o.write(c.data(), c.size());
        };
        touch(base + "/a.cppm", "int a = 1;\n");
        touch(base + "/b.txt",  "hello world\n");
        touch(base + "/sub/c.cppm", "void f() {}\nint sub = 7;\n");
        touch(base + "/sub/d.log", "ignored\n");

        GlobOptions go;
        go.pattern   = "*.cppm";
        go.base_dir  = base;
        go.max_results = 50;
        auto gr = Glob(go);
        CHECK(gr.error.empty(), "glob error empty: %s", gr.error.c_str());
        CHECK(gr.matches.size() == 2, "glob *.cppm matched 2 got=%zu", gr.matches.size());

        GrepOptions gpro;
        gpro.path          = base;
        gpro.pattern       = "int";
        gpro.fixed_strings = true;
        gpro.max_results   = 10;
        auto grep_r = Grep(gpro);
        CHECK(grep_r.error.empty(), "grep error empty: %s", grep_r.error.c_str());
        CHECK(grep_r.matches.size() == 2, "grep literal 'int' count=2 got=%zu", grep_r.matches.size());

        std::filesystem::remove_all(base, ec);
    }

    if (g_failed == 0) {
        std::puts("ALL PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(s) FAILED\n", g_failed);
    return 1;
}
