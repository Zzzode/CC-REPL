/// @file tools_e2e.cpp
/// @brief Phase 3-B/C end-to-end test combining Bash fork/exec with the
///        real filesystem primitives (Write / Read / Edit / Glob / Grep).
///
/// Scenario:
///   1. Create a scratch directory with 3 data files (WriteFile, mkdir).
///   2. Use WriteFile to drop a shell script that transforms them into a
///      single aggregated report (shuffle / line counts / checksum via `cksum`).
///   3. ExecuteBash runs the script; assertions on exit_code / stdout / stderr.
///   4. ReadFile loads the report and checks expected headers + data counts.
///   5. EditFile patches a marker in the report → re-reads to verify.
///   6. Glob enumerates *.txt in the scratch dir → exact match count.
///   7. Grep for literal "RECORD" in the aggregated file → exact match count.
/// Exit 0 on all assertions passed; print one FAIL line per broken assertion
/// and exit non-zero otherwise.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <filesystem>
#include <iostream>

import cc.tools.bash.impl;
import cc.tools.files.impl;

using namespace cc::tools::bash::impl;
using namespace cc::tools::files::impl;
namespace fs = std::filesystem;

static int g_fail = 0;
#define FAIL(...) do { \
    ++g_fail; \
    std::fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\n"); \
} while(0)
#define CHECK(expr, ...) do { \
    if (!(expr)) { FAIL(__VA_ARGS__); } \
    else { std::printf("  OK: "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

int main() {
    std::puts("=== Phase 3-B/C E2E: Bash x Files ===");

    // 1. Scratch dir.
    const fs::path scratch = fs::temp_directory_path() / "cc_phase3_e2e";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch / "data", ec);
    if (ec) { FAIL("create scratch: %s", ec.message().c_str()); return 1; }

    auto write_str = [&](const fs::path& p, std::string_view c) {
        WriteOptions o;
        o.path    = p.string();
        o.content = std::string{c};
        o.mode    = WriteMode::Overwrite;
        return WriteFile(std::move(o));
    };

    const std::string data1 =
        "# sample dataset A\n"
        "RECORD id=101 user=alice score=88\n"
        "RECORD id=102 user=bob   score=75\n"
        "RECORD id=103 user=carol score=92\n";
    const std::string data2 =
        "# sample dataset B\n"
        "RECORD id=201 user=dave score=60\n"
        "RECORD id=202 user=erin score=80\n";
    const std::string data3 =
        "# sample dataset C\n"
        "RECORD id=301 user=frank score=95\n";
    auto w1 = write_str(scratch / "data" / "a.txt", data1); CHECK(w1.ok, "write a.txt");
    auto w2 = write_str(scratch / "data" / "b.txt", data2); CHECK(w2.ok, "write b.txt");
    auto w3 = write_str(scratch / "data" / "c.txt", data3); CHECK(w3.ok, "write c.txt");

    // 2. Shell script aggregates inputs into report.txt.
    const fs::path script_path = scratch / "aggregate.sh";
    const fs::path report_path = scratch / "report.txt";
    const fs::path checksums_path = scratch / "checksums.txt";
    std::string script =
        "#!/bin/sh\n"
        "set -eu\n"
        "echo '=== AGGREGATED REPORT PHASE3 ===' > " + report_path.string() + "\n"
        "echo 'DATASETS: A B C' >> " + report_path.string() + "\n"
        "for f in " + (scratch / "data").string() + "/*.txt; do\n"
        "  base=$(basename \"$f\")\n"
        "  lines=$(grep -c '^RECORD' \"$f\" || true)\n"
        "  echo \"[$base] records=$lines\" >> " + report_path.string() + "\n"
        "  cksum \"$f\"\n"
        "done > " + checksums_path.string() + " 2>&1\n"
        "total=$(awk -F: '/^RECORD/ {c++} END {print c+0}' " + (scratch / "data").string() + "/*.txt)\n"
        "echo \"TOTAL_RECORDS=$total\" >> " + report_path.string() + "\n"
        "echo '=== END REPORT ===' >> " + report_path.string() + "\n"
        "echo 'e2e-done stdout'\n"
        "echo 'e2e-warn stderr' 1>&2\n";
    {
        WriteOptions o;
        o.path    = script_path.string();
        o.content = script;
        o.mode    = WriteMode::Overwrite;
        o.mode_bits = 0755;
        auto r = WriteFile(std::move(o));
        CHECK(r.ok, "write aggregate.sh");
    }

    // 3. Bash run.
    {
        BashOptions o;
        o.command         = "/bin/sh " + script_path.string();
        o.timeout_sec     = 10;
        o.allow_fail      = false;
        o.combine_streams = false;
        auto r = ExecuteBash(std::move(o));
        CHECK(r.exit_code == 0, "bash exit=0 got=%d (%s)", r.exit_code, r.error_reason.c_str());
        CHECK(!r.timed_out, "not timed out");
        CHECK(r.stdout_only.find("e2e-done stdout") != std::string::npos,
              "stdout contains banner: '%s'", r.stdout_only.c_str());
        CHECK(r.stderr_only.find("e2e-warn stderr") != std::string::npos,
              "stderr contains warn: '%s'", r.stderr_only.c_str());
    }

    // 4. Read report + assertions.
    std::string report;
    {
        ReadOptions o; o.path = report_path.string();
        auto rr = ReadFile(o);
        CHECK(rr.ok, "read report.txt ok: %s", rr.error.c_str());
        CHECK(rr.lines_read >= 6, "report has enough lines got=%d", rr.lines_read);
        report = rr.content;
    }
    auto contains = [](std::string_view h, std::string_view n) {
        return h.find(n) != std::string_view::npos;
    };
    CHECK(contains(report, "=== AGGREGATED REPORT PHASE3 ==="), "report header");
    CHECK(contains(report, "[a.txt] records=3"), "a.txt count got='%s'", report.c_str());
    CHECK(contains(report, "[b.txt] records=2"), "b.txt count");
    CHECK(contains(report, "[c.txt] records=1"), "c.txt count");
    CHECK(contains(report, "TOTAL_RECORDS=6"), "total records aggregate");
    CHECK(contains(report, "=== END REPORT ==="), "report footer");

    // 5. EditFile.
    {
        EditOptions o;
        o.path       = report_path.string();
        o.old_string = "TOTAL_RECORDS=6";
        o.new_string = "TOTAL_RECORDS=6  # verified-by-e2e";
        auto er = EditFile(o);
        CHECK(er.ok && er.replacements == 1, "edit TOTAL_RECORDS marker rpl=%d err=%s",
              er.replacements, er.error.c_str());
    }
    {
        ReadOptions o; o.path = report_path.string();
        auto rr = ReadFile(o);
        CHECK(contains(rr.content, "TOTAL_RECORDS=6  # verified-by-e2e"),
              "edit was persisted");
    }

    // 6. Glob *.txt (data dir only, pattern *.txt -> a/b/c).
    {
        GlobOptions o;
        o.pattern     = "*.txt";
        o.base_dir    = (scratch / "data").string();
        o.max_results = 100;
        auto gr = Glob(o);
        CHECK(gr.error.empty(), "glob error empty: %s", gr.error.c_str());
        CHECK(gr.matches.size() == 3, "glob *.txt in data => 3 got=%zu", gr.matches.size());
    }
    // Whole scratch root *.txt (a/b/c + report + checksums) = 5.
    {
        GlobOptions o;
        o.pattern     = "*.txt";
        o.base_dir    = scratch.string();
        o.max_results = 100;
        auto gr = Glob(o);
        CHECK(gr.matches.size() == 5, "glob *.txt recursive => 5 got=%zu", gr.matches.size());
    }

    // 7. Grep literal RECORD across scratch data dir.
    {
        GrepOptions o;
        o.path          = (scratch / "data").string();
        o.pattern       = "RECORD";
        o.fixed_strings = true;
        o.max_results   = 100;
        auto gr = Grep(o);
        CHECK(gr.files_scanned == 3, "grep scanned 3 files got=%d", gr.files_scanned);
        CHECK(gr.matches.size() == 6, "grep RECORD count=6 got=%zu", gr.matches.size());
        // Every line_no should be > 0 and file should end in .txt
        int bad = 0;
        for (auto& m : gr.matches) {
            if (m.line_no <= 0) ++bad;
            if (m.file.size() < 4 || m.file.substr(m.file.size()-4) != ".txt") ++bad;
        }
        CHECK(bad == 0, "all grep matches valid got bad=%d", bad);
    }

    // Extra: Grep with regex pattern matching id=XXX numeric values.
    {
        GrepOptions o;
        o.path          = (scratch / "data").string();
        o.pattern       = "id=[0-9]+ user=[a-z]+";
        o.fixed_strings = false;
        o.case_sensitive = true;
        o.max_results    = 100;
        auto gr = Grep(o);
        CHECK(gr.matches.size() == 6, "grep regex id=... => 6 got=%zu", gr.matches.size());
    }

    // Tidy.
    fs::remove_all(scratch, ec);

    if (g_fail == 0) {
        std::puts("E2E ALL PASSED");
        return 0;
    }
    std::fprintf(stderr, "E2E %d assertion(s) FAILED\n", g_fail);
    return 1;
}
