/// @file phase3_permission_smoke.cpp
/// @brief Standalone smoke driver for Phase 3-G PermissionResolver + PermissionGate.
///
/// Exercises all 5 acceptance cases (plus extra edge cases) and exits 0 on
/// success, 1 on assertion failure.
///
/// Not registered as a ctest — this is a standalone task-scope validation,
/// identical in spirit to the Phase 3-A SSE mock driver.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

import cc.hooks.permission_resolver;
import cc.hooks.tool_permission_gate;

using namespace cc::hooks::permission;

namespace {

int failures = 0;

int Check(const char* label, bool cond, const std::string& detail = "") {
    if (!cond) {
        ++failures;
        std::fprintf(stderr, "FAIL  %-60s %s\n", label, detail.c_str());
        return 1;
    }
    std::fprintf(stdout, "ok    %-60s %s\n", label, detail.c_str());
    return 0;
}

std::string ReprDecision(Decision d) {
    return std::string(DecisionName(d)) + "(" +
           std::to_string(static_cast<int>(d)) + ")";
}

}  // namespace

int main() {
    // -----------------------------------------------------------------------
    // Case 1: Resolver empty + Low+sandboxed → AllowOnce
    // -----------------------------------------------------------------------
    {
        PermissionResolver r;
        PermissionRequest req;
        req.tool_name  = "Bash";
        req.action     = ActionKind::Execute;
        req.affected_paths = {"/tmp/job.sh"};
        req.risk       = RiskLevel::Low;
        req.sandboxed  = true;
        req.extra      = "echo hello";
        const Decision d = r.Resolve(req);
        Check("Case1: empty resolver + low risk + sandboxed = AllowOnce",
              d == Decision::AllowOnce,
              "got=" + ReprDecision(d));
    }

    // -----------------------------------------------------------------------
    // Case 2: Resolver empty + High risk → Deny
    // -----------------------------------------------------------------------
    {
        PermissionResolver r;
        PermissionRequest req;
        req.tool_name  = "Bash";
        req.action     = ActionKind::Execute;
        req.risk       = RiskLevel::High;
        req.sandboxed  = false;
        req.extra      = "rm -rf /";
        const Decision d = r.Resolve(req);
        Check("Case2: empty resolver + high risk + not sandboxed = Deny",
              d == Decision::Deny,
              "got=" + ReprDecision(d));
    }
    // Sub-cases: Medium, Critical, !sandboxed also return Deny
    {
        PermissionResolver r;
        PermissionRequest req;
        req.tool_name = "Bash";
        req.risk = RiskLevel::Medium;
        req.sandboxed = false;
        Check("Case2b: Medium risk, no sandbox → Deny",
              r.Resolve(req) == Decision::Deny);
        req.risk = RiskLevel::Critical;
        Check("Case2c: Critical risk, no sandbox → Deny",
              r.Resolve(req) == Decision::Deny);
        req.risk = RiskLevel::Low;
        req.sandboxed = false;
        Check("Case2d: Low risk, no sandbox → Deny (require prompt)",
              r.Resolve(req) == Decision::Deny);
    }

    // -----------------------------------------------------------------------
    // Case 3: CacheAlways(Bash, /tmp/**, AlwaysAllow) + Resolve(Bash, /tmp/x.sh)
    // -----------------------------------------------------------------------
    {
        PermissionResolver r;
        r.CacheAlways("Bash", "/tmp/**", Decision::AlwaysAllow);
        PermissionRequest req;
        req.tool_name = "Bash";
        req.action    = ActionKind::Execute;
        req.affected_paths = {"/tmp/x.sh"};
        req.risk      = RiskLevel::High;  // high risk, but cache overrides
        req.sandboxed = false;
        const Decision d = r.Resolve(req);
        Check("Case3: CacheAlways(Bash,/tmp/**,Allow) → AlwaysAllow matches /tmp/x.sh",
              d == Decision::AlwaysAllow,
              "got=" + ReprDecision(d));

        // Non-matching path (outside /tmp) must fall back to default
        PermissionRequest req2;
        req2.tool_name = "Bash";
        req2.affected_paths = {"/etc/passwd"};
        req2.risk = RiskLevel::High;
        const Decision d2 = r.Resolve(req2);
        Check("Case3b: same rule does NOT match /etc/passwd → Deny",
              d2 == Decision::Deny,
              "got=" + ReprDecision(d2));

        // Different tool name + same path → no hit
        PermissionRequest req3;
        req3.tool_name = "FileWrite";
        req3.affected_paths = {"/tmp/x.log"};
        req3.risk = RiskLevel::Medium;
        const Decision d3 = r.Resolve(req3);
        Check("Case3c: FileWrite does not match Bash rule → Deny",
              d3 == Decision::Deny,
              "got=" + ReprDecision(d3));
    }

    // -----------------------------------------------------------------------
    // Case 4: CacheToJson() roundtrip back → same decisions
    // -----------------------------------------------------------------------
    {
        PermissionResolver r1;
        r1.CacheAlways("Bash",      "/tmp/**",              Decision::AlwaysAllow);
        r1.CacheAlways("Bash",      "/root/**",             Decision::AlwaysDeny);
        r1.CacheAlways("FileWrite", "/Users/bytedance/**", Decision::AlwaysAllow);
        Check("Case4a: three cache entries stored", r1.cache_size() == 3);

        const std::string json = r1.CacheToJson();
        Check("Case4b: JSON non-empty", !json.empty(), "json=" + json);

        PermissionResolver r2;
        const bool ok = r2.CacheFromJson(json);
        Check("Case4c: CacheFromJson returns true", ok, "json=" + json);
        Check("Case4d: r2 also has 3 entries", r2.cache_size() == 3);

        // Each decision must reproduce
        PermissionRequest ra; ra.tool_name = "Bash"; ra.affected_paths = {"/tmp/a"}; ra.risk = RiskLevel::High;
        PermissionRequest rb; rb.tool_name = "Bash"; rb.affected_paths = {"/root/.ssh/id_rsa"}; rb.risk = RiskLevel::High;
        PermissionRequest rc; rc.tool_name = "FileWrite"; rc.affected_paths = {"/Users/bytedance/a.c"}; rc.risk = RiskLevel::Medium;
        Check("Case4e: roundtrip AlwaysAllow /tmp/**",
              r2.Resolve(ra) == Decision::AlwaysAllow);
        Check("Case4f: roundtrip AlwaysDeny  /root/**",
              r2.Resolve(rb) == Decision::Deny);  // resolver collapses AlwaysDeny→Deny
        Check("Case4g: roundtrip AlwaysAllow /Users/bytedance/**",
              r2.Resolve(rc) == Decision::AlwaysAllow);

        // Malformed JSON must not crash and must return false.
        PermissionResolver r3;
        Check("Case4h: empty JSON string → false",
              !r3.CacheFromJson(""));
        Check("Case4i: garbage JSON string → false",
              !r3.CacheFromJson("not json at all"));
        Check("Case4j: version bump → false (forward-incompat)",
              !r3.CacheFromJson("{\"version\":999,\"entries\":[]}"));
        Check("Case4k: malformed entries array → false",
              !r3.CacheFromJson("{\"version\":1,\"entries\":[{}]"));
    }

    // -----------------------------------------------------------------------
    // Case 5: glob "**/*.cppm" matches deeply-nested path
    // -----------------------------------------------------------------------
    {
        const auto& pat = std::string_view("**/*.cppm");
        const bool ok1 = MatchGlob(pat, "src/ui/dialogs/help_v2.cppm");
        const bool ok2 = MatchGlob(pat, "help_v2.cppm");
        const bool ok3 = MatchGlob(pat, "src/x.cpp");     // wrong suffix
        const bool ok4 = MatchGlob(pat, "src/x.cppm/foo"); // wrong position
        Check("Case5a: **/*.cppm matches src/ui/dialogs/help_v2.cppm", ok1);
        Check("Case5b: **/*.cppm matches help_v2.cppm (no dir)",       ok2);
        Check("Case5c: **/*.cppm rejects src/x.cpp (suffix)",         !ok3);
        Check("Case5d: **/*.cppm rejects src/x.cppm/foo (middle)",    !ok4);

        // Single-segment * does not cross dir boundary.
        Check("Case5e: *.cppm rejects src/a.cppm (dir separator)",
              !MatchGlob("*.cppm", "src/a.cppm"));
        Check("Case5f: src/*.cppm matches src/a.cppm but NOT src/d/a.cppm",
              MatchGlob("src/*.cppm", "src/a.cppm") &&
              !MatchGlob("src/*.cppm", "src/d/a.cppm"));
    }

    // -----------------------------------------------------------------------
    // Extra: Decision enum numeric values frozen (ABI guard).
    // -----------------------------------------------------------------------
    {
        const auto raw = [](Decision d) { return static_cast<int>(d); };
        Check("ABI: AllowOnce == 0", raw(Decision::AllowOnce)   == 0);
        Check("ABI: AlwaysAllow == 1", raw(Decision::AlwaysAllow) == 1);
        Check("ABI: Deny == 2",        raw(Decision::Deny)        == 2);
        Check("ABI: AlwaysDeny == 3",  raw(Decision::AlwaysDeny)  == 3);
        Check("ABI: Abort == 4",       raw(Decision::Abort)       == 4);
    }

    // -----------------------------------------------------------------------
    // Extra: PermissionGate wires resolver + interactive prompt correctly.
    // -----------------------------------------------------------------------
    {
        PermissionGate gate;
        // No interactive prompt installed → safe defaults.
        PermissionRequest low_sb; low_sb.tool_name = "Bash"; low_sb.risk = RiskLevel::Low; low_sb.sandboxed = true;
        PermissionRequest high;   high.tool_name   = "Bash"; high.risk   = RiskLevel::High;

        Check("Gate1: no prompt + low+sbox → AllowToolExecute → true",
              gate.AllowToolExecute(low_sb));
        Check("Gate2: no prompt + high risk → AllowToolExecute → false",
              !gate.AllowToolExecute(high));

        // Install a prompt that always returns AlwaysAllow for /tmp/ paths.
        gate.interactive_prompt = [](const PermissionRequest& req) -> Decision {
            if (!req.affected_paths.empty() &&
                MatchGlob("/tmp/**", req.affected_paths.front())) {
                return Decision::AlwaysAllow;
            }
            return Decision::Deny;
        };
        PermissionRequest tmp_path;
        tmp_path.tool_name = "FileWrite";
        tmp_path.action    = ActionKind::Write;
        tmp_path.affected_paths = {"/tmp/notes.txt"};
        tmp_path.risk     = RiskLevel::Critical;  // risk doesn't matter — prompt overrides
        const bool ok_gate = gate.AllowToolExecute(tmp_path);
        Check("Gate3: prompt returns AlwaysAllow → true", ok_gate);

        // Cache should now contain the rule.
        PermissionRequest same_again;
        same_again.tool_name = "FileWrite";
        same_again.affected_paths = {"/tmp/foo"};
        same_again.risk = RiskLevel::High;
        Decision d_prompt_used = gate.ResolveWithPrompt(same_again);
        Check("Gate4: same (FileWrite,/tmp/**) now auto-approves via cache",
              d_prompt_used == Decision::AlwaysAllow,
              "got=" + ReprDecision(d_prompt_used));

        // Non-/tmp path → prompt denies.
        PermissionRequest outside;
        outside.tool_name = "FileWrite";
        outside.affected_paths = {"/etc/hosts"};
        outside.risk = RiskLevel::High;
        Check("Gate5: path outside /tmp → prompt Deny",
              !gate.AllowToolExecute(outside));

        // Abort propagates as "don't execute".
        gate.interactive_prompt = [](auto&&) { return Decision::Abort; };
        PermissionRequest any; any.risk = RiskLevel::Medium;
        Check("Gate6: Abort → AllowToolExecute == false",
              !gate.AllowToolExecute(any));

        // Broken prompt (throws) → fail closed.
        gate.interactive_prompt = [](auto&&) -> Decision {
            throw std::runtime_error("UI exploded");
            return Decision::Deny;
        };
        PermissionRequest any2; any2.risk = RiskLevel::Medium;
        Check("Gate7: throwing prompt → fail closed",
              !gate.AllowToolExecute(any2));
    }

    // -----------------------------------------------------------------------
    // Extra: glob edge cases
    // -----------------------------------------------------------------------
    {
        Check("Glob edge1: empty pattern matches empty string",
              MatchGlob("", ""));
        Check("Glob edge2: empty pattern rejects non-empty string",
              !MatchGlob("", "a"));
        Check("Glob edge3: ? matches single non-/ char",
              MatchGlob("a?c", "abc") && !MatchGlob("a?c", "a/c") &&
              !MatchGlob("a?c", "ac"));
        Check("Glob edge4: a/**/z matches a/z + a/b/z + a/b/c/z",
              MatchGlob("a/**/z", "a/z") &&
              MatchGlob("a/**/z", "a/b/z") &&
              MatchGlob("a/**/z", "a/b/c/z"));
    }

    // -----------------------------------------------------------------------
    // Final summary
    // -----------------------------------------------------------------------
    if (failures == 0) {
        std::printf("\n=== Phase 3-G (PermissionResolver + PermissionGate) PASSED ===\n");
        return 0;
    }
    std::fprintf(stderr, "\n*** %d assertion(s) failed ***\n", failures);
    return 1;
}
