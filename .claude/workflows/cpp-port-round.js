export const meta = {
  name: 'cpp-port-round',
  description: 'TS to CPP faithful-port round. Pipeline: audit-driven gap discovery; 3-judge feasibility/priority/risk panel; top-gap implementation; cmake build + scoped ctest + UPDATE_GOLDENS verification. Loops until budget exhausted or no fresh gaps.',
  whenToUse: 'Use when continuing the cpp_migration faithful-port work. Accepts a scope arg (subsystem name, TS path filter, P-level, or "all") and autonomously picks, implements, and verifies one or more TS-to-CPP alignment gaps.',
  phases: [
    { title: 'Discover', detail: 'load audit_round7 JSON and inventory scripts; filter by caller scope' },
    { title: 'Judge', detail: '3 independent judges per shortlisted gap; composite ranking' },
    { title: 'Implement', detail: 'editor agents per top-K doable gap; add/refresh goldens' },
    { title: 'Verify', detail: 'cmake Debug+Release build; scoped ctest; golden round-trip; debugger on regressions' },
    { title: 'Loop', detail: 'optional atomic conventional-commits grouping + push; return ranked remainder' },
  ],
};

// ============================================================
// Structured-output schemas (JSON Schema, validated by agent())
// ============================================================

const GAP_SCHEMA = {
  type: 'object',
  required: ['id', 'subsystem', 'severity', 'title', 'ts_path', 'cpp_path',
             'cpp_status', 'evidence', 'fix_estimate_lines', 'is_user_visible',
             'dependencies'],
  properties: {
    id:               { type: 'string', description: 'gap id from audit JSON (e.g. UI-P0-3)' },
    subsystem:        { type: 'string', enum: ['UI-Audit-Report', 'UI-Messages',
                                                'UI-Prompt', 'UI-Dialogs|DesignTokens',
                                                'CppMigrationAudit'] },
    severity:         { type: 'string', enum: ['P0', 'P1', 'P2', 'P3', 'unknown'] },
    title:            { type: 'string', description: 'one-line gap title' },
    ts_path:          { type: 'string', description: 'TS reference file, e.g. src/components/messages/AssistantTextMessage.tsx' },
    cpp_path:         { type: 'string', description: 'CPP target file or directory to touch' },
    cpp_status:       { type: 'string', description: '"aligned" / "partial" / "missing" / "divergent"' },
    evidence:         { type: 'string', description: 'short rationale from audit JSON' },
    fix_estimate_lines: { type: 'integer', minimum: 0, maximum: 2000 },
    is_user_visible:  { type: 'boolean' },
    dependencies:     { type: 'array', items: { type: 'string' },
                        description: 'other gap ids that must land first (or empty)' },
  },
};

const JUDGEMENT_SCHEMA = {
  type: 'object',
  required: ['gap_id', 'doable', 'priority_score', 'risk_level',
             'prerequisites', 'estimated_hours', 'notes'],
  properties: {
    gap_id:           { type: 'string' },
    doable:           { type: 'boolean',
                        description: 'true if this gap can be implemented NOW given repo state' },
    priority_score:   { type: 'integer', minimum: 0, maximum: 100,
                        description: '100 = drop everything and do this. Weight: P0=90, user_visible=+15, small=+10, missing>divergent>partial.' },
    risk_level:       { type: 'string', enum: ['low', 'medium', 'high', 'extreme'] },
    prerequisites:    { type: 'array', items: { type: 'string' },
                        description: 'other gap ids, CMake changes, or new module files needed first' },
    estimated_hours:  { type: 'number', minimum: 0, maximum: 24 },
    notes:            { type: 'string' },
  },
};

const IMPL_REPORT_SCHEMA = {
  type: 'object',
  required: ['gap_id', 'files_written', 'files_modified', 'lines_added', 'lines_removed',
             'golden_files_updated', 'notes'],
  properties: {
    gap_id:                { type: 'string' },
    files_written:         { type: 'array', items: { type: 'string' } },
    files_modified:        { type: 'array', items: { type: 'string' } },
    lines_added:           { type: 'integer', minimum: 0 },
    lines_removed:         { type: 'integer', minimum: 0 },
    golden_files_updated:  { type: 'array', items: { type: 'string' },
                             description: 'golden .txt files newly added or refreshed' },
    notes:                 { type: 'string' },
  },
};

const VERIFY_REPORT_SCHEMA = {
  type: 'object',
  required: ['gap_id', 'build_rc', 'build_warning_count',
             'ctest_scope', 'ctest_total', 'ctest_passed', 'ctest_skipped', 'ctest_failed',
             'ctest_new_fails', 'golden_update_needed', 'verdict'],
  properties: {
    gap_id:                { type: 'string' },
    build_rc:              { type: 'integer', description: 'ninja exit code; 0 = success' },
    build_warning_count:   { type: 'integer', minimum: 0 },
    ctest_scope:           { type: 'string', description: '-R regex passed to ctest' },
    ctest_total:           { type: 'integer', minimum: 0 },
    ctest_passed:          { type: 'integer', minimum: 0 },
    ctest_skipped:         { type: 'integer', minimum: 0 },
    ctest_failed:          { type: 'integer', minimum: 0 },
    ctest_new_fails:       { type: 'array', items: { type: 'string' },
                             description: 'failing test names NOT on the pre-existing flake list' },
    golden_update_needed:  { type: 'boolean', description: 'true if ctest failed on goldens and UPDATE_GOLDENS=1 would fix it' },
    verdict:               { type: 'string', enum: ['PASS', 'BUILD_FAIL', 'TEST_REGRESSION',
                                                     'GOLDEN_REFRESH_NEEDED', 'UNKNOWN'] },
  },
};

const COMMIT_PLAN_SCHEMA = {
  type: 'object',
  required: ['commits'],
  properties: {
    commits: {
      type: 'array',
      items: {
        type: 'object',
        required: ['subject', 'files'],
        properties: {
          subject: { type: 'string', description: 'conventional commit subject line, e.g. fix(cpp-migration,R7): assistant BLACK_CIRCLE minWidth=2' },
          files:   { type: 'array', items: { type: 'string' }, description: 'absolute paths grouped into this commit' },
          body:    { type: 'string', description: 'optional multi-line body' },
        },
      },
    },
  },
};

// ----- Known pre-existing flaky / timing-out test names. Tests on this list
// failing after a change are NOT reported as regressions. Keep in sync with
// memory + the last T1-baseline ctest run.
const PREEXISTING_FAILS = new Set([
  'FTXUIIntegration.VerboseIndicator',
  'ReplScreen.PromptInputParksHiddenNativeCursorAtCaret',
  'SandboxPermissionEvents.ArrowDown_WrapsFocusThroughAllOptions',
  'SandboxPermissionEvents.ArrowUp_WrapsFocusBackwards',
  'SandboxPermissionEvents.VimJK_MoveFocus',
  'SandboxPermissionEvents.ManagedDomainsOnly_TwoOptionsWrapCorrectly',
  'AppRuntime.SkillsDialogDismissOrderDebug',
  'BridgeDaemon.PipesHeadlessChildStdinAndCapturesStdout',
]);

// ----- Default severity → base priority (judges add weights)
const SEVERITY_BASE = { P0: 90, P1: 65, P2: 35, P3: 10, unknown: 20 };

// ==================================================================
// PHASE 1 — Discover: load audit JSON, normalize rows, apply filters
// ==================================================================
phase('Discover');

log('Loading audit_round7_full_report.json (cpp_migration/docs/)…');
const audit = await agent(
  'Read /Users/bytedance/Develop/CC-REPL/cpp_migration/docs/audit_round7_full_report.json. ' +
  'Parse it and return the full list of "gap" entries (i.e. entries where cpp_status != ' +
  '"aligned" / status != aligned — whatever the JSON uses). Return JSON only. ' +
  'If the file is missing or empty, fall back to walking ' +
  '/Users/bytedance/Develop/CC-REPL/cpp_migration/docs/ and reading the closest ' +
  'alternative audit JSON (e.g. prompt_audit_result.json).',
  {
    label: 'audit-loader',
    phase: 'Discover',
    schema: {
      type: 'object',
      required: ['gaps'],
      properties: { gaps: { type: 'array', items: GAP_SCHEMA } },
    },
    effort: 'medium',
  }
);

// Scope: a string passed from the caller via global `args`. Allowed shapes:
//   undefined / 'all' / 'P0' / 'P1' / 'P0+P1' /
//   subsys:'UI-Messages' / ts:'src/components/messages' / cpp:'ui/messages'
let rawScope = typeof args === 'string' ? args : (args && args.scope) || 'all';
let scope = String(rawScope);
log(`Scope filter = "${scope}"`);

let shortlist = (audit && audit.gaps) ? audit.gaps.slice() : [];

// ---- scope filter helpers ----
const bySeverity = (s, list) => s === 'all' ? list : list.filter(g => {
  const need = new Set(s.split('+').map(x => x.trim()));
  return need.has(g.severity);
});
shortlist = bySeverity(scope, shortlist);

if (scope.startsWith('subsys:')) {
  const s = scope.slice(7);
  shortlist = shortlist.filter(g => g.subsystem === s);
} else if (scope.startsWith('ts:')) {
  const p = scope.slice(3);
  shortlist = shortlist.filter(g => (g.ts_path || '').includes(p));
} else if (scope.startsWith('cpp:')) {
  const p = scope.slice(4);
  shortlist = shortlist.filter(g => (g.cpp_path || '').includes(p));
}

// ---- de-dup by id ----
const seen = new Set();
shortlist = shortlist.filter(g => g.id && !seen.has(g.id) && (seen.add(g.id), true));

// ---- remove gaps whose dependencies are not already marked aligned
//      (best-effort: dependencies refer to other gap ids we haven't seen — treat as unknown)
shortlist.sort((a, b) =>
  (SEVERITY_BASE[b.severity] || 0) + (b.is_user_visible ? 15 : 0)
  - ((SEVERITY_BASE[a.severity] || 0) + (a.is_user_visible ? 15 : 0))
);

// budget cap on shortlist size (keeps judge panel work bounded)
const SHORTLIST_MAX = 12;
if (shortlist.length > SHORTLIST_MAX) {
  log(`Truncating shortlist ${shortlist.length} → ${SHORTLIST_MAX} (top by severity+visibility)`);
  shortlist = shortlist.slice(0, SHORTLIST_MAX);
}

if (!shortlist.length) {
  log('No actionable gaps after scope filter. Round done.');
  await agent('No gaps. Return empty shortlist.', {
    label: 'done', phase: 'Discover', effort: 'low',
    schema: { type: 'object', required: [], properties: {} },
  });
  return { done: true, reason: 'no_gaps_after_filter', shortlist: [] };
}

log(`Shortlist prepared: ${shortlist.length} gaps. Entering Judge phase.`);

// ==================================================================
// PHASE 2 — Judge panel: 3 independent judges per gap → composite rank
// ==================================================================
phase('Judge');

const LENSES = [
  { name: 'feasibility',
    prompt: 'Judge FEASIBILITY. Focus on: (1) is the TS file readable & does the CPP ' +
            'target already have a skeleton to extend? (2) are all dependencies (new modules, ' +
            'CMake changes, golden harness) already present? (3) is this a self-contained, ' +
            '≤800 LOC change? Set doable=false if the gap needs ≥3 new module files or needs ' +
            'a cross-module refactor not listed in prerequisites. Score higher for small, ' +
            'isolated, golden-verifiable gaps.' },
  { name: 'priority',
    prompt: 'Judge PRIORITY. Weight: P0=90 base, P1=65, P2=35, P3=10. Add: ' +
            '+20 user-visible, +10 low LOC (<200), +15 touches dialogs/prompt/status-line ' +
            '(high-frequency surfaces). Deduct: -20 blocked by dependencies, -10 needs new ' +
            'golden harness infrastructure. 100 = do this gap NOW.' },
  { name: 'risk',
    prompt: 'Judge RISK. risk_level = low (purely additive, touches 1 module + 1 golden), ' +
            'medium (3–5 files, rewires existing renderer), high (cross-cutting — e.g. ' +
            'QueryEngine or component lifetime), extreme (any change to bridge daemon / ' +
            'config schema migration / native IPC). Deduct heavily from priority_score if ' +
            'risk ≥ high without a compelling severity.' },
];

const judgements = await pipeline(
  shortlist,
  gap => parallel(LENSES.map(l => () => agent(
    `You are the "${l.name}"-lens judge. Evaluate this TS→CPP porting gap and output ONLY ' +
    'the JUDGEMENT_SCHEMA object. Keep all numbers consistent with your reasoning.\n\n` +
    l.prompt + `\n\nGAP under review:\n` + JSON.stringify(gap, null, 2),
    {
      label: `judge-${l.name}:${gap.id}`,
      phase: 'Judge',
      schema: JUDGEMENT_SCHEMA,
      effort: 'low',
    }
  ))).then(votes => {
    // --- composite: average priority (dropping top/bottom if 3 votes), require ≥2 "doable" ---
    const valid = votes.filter(Boolean);
    const doable_count = valid.filter(v => v.doable).length;
    const scores = valid.map(v => v.priority_score).sort((a,b)=>a-b);
    const trimmed = scores.length >= 3 ? scores.slice(1, -1) : scores;
    const avg = trimmed.length ? trimmed.reduce((a,b)=>a+b,0)/trimmed.length : 0;
    const worst_risk = valid.reduce((acc,v)=>{
      const rank = {low:0,medium:1,high:2,extreme:3};
      return Math.max(acc, rank[v.risk_level] ?? 0);
    }, 0);
    const risk_name = ['low','medium','high','extreme'][worst_risk];
    const prereqs = Array.from(new Set(valid.flatMap(v => v.prerequisites || [])));
    return {
      gap,
      votes: valid,
      composite_doable: doable_count >= 2,
      composite_priority: Math.round(avg),
      composite_risk: risk_name,
      composite_prerequisites: prereqs,
      composite_hours: +(valid.reduce((a,v)=>a+(v.estimated_hours||0),0) / Math.max(1,valid.length)).toFixed(1),
    };
  })
);

// ---- final ranked list: doable first, priority desc, then blocked low-risk ----
const ranked = judgements
  .filter(Boolean)
  .sort((a, b) => {
    if (a.composite_doable !== b.composite_doable) return a.composite_doable ? -1 : 1;
    if (b.composite_priority !== a.composite_priority) return b.composite_priority - a.composite_priority;
    const rk = {low:0,medium:1,high:2,extreme:3};
    return (rk[a.composite_risk]||0) - (rk[b.composite_risk]||0);
  });

log(`Judge panel complete. Top-3:\n` +
    ranked.slice(0,3).map((r,i) =>
      `  ${i+1}. ${r.gap.id} [${r.gap.severity}] doable=${r.composite_doable} ` +
      `priority=${r.composite_priority} risk=${r.composite_risk} — ${r.gap.title}`
    ).join('\n'));

// ---- Pick up to K gaps for Implement phase (bounded by budget; sequential by default
//      to keep build/CTest contention off; caller can raise K via args).
const K = Math.max(1, Math.min(3, (args && args.parallelism) || 1));
const toImplement = ranked
  .filter(r => r.composite_doable)
  .slice(0, K);

if (!toImplement.length) {
  log('No doable gaps after judge panel. Recommending highest-priority blocked gap.');
  const blocked = ranked[0];
  return {
    done: true,
    reason: 'no_doable_gaps',
    top_blocked: blocked && {
      id: blocked.gap.id,
      title: blocked.gap.title,
      why_blocked: blocked.votes.map(v => `${v.risk_level}:${v.notes}`).join('; '),
      prerequisites: blocked.composite_prerequisites,
    },
    shortlist: ranked.map(r => ({
      id: r.gap.id, title: r.gap.title, priority: r.composite_priority,
      doable: r.composite_doable, risk: r.composite_risk,
    })),
  };
}

// ==================================================================
// PHASE 3 — Implement (editor agents; one per shortlisted gap)
//          + PHASE 4 — Verify (build + ctest + golden trip)
// ==================================================================
phase('Implement');
log(`Implement phase — K=${toImplement.length}  (sequential build/CTest per gap)`);

// ---- GROUND RULES injected into every implementor agent prompt ----
const GROUND_RULES = [
  // From memory: ui-port-ts-is-the-standard
  'RULE #1 (TS = AUTHORITY): The TS source at `ts_path` is the reference. Read it FIRST. ' +
    'Do NOT ask "is this OK?"; match TS exactly. ' +
    'Add // TS REF: file:line comments in CPP code where faithful ports are non-obvious.',
  'RULE #2 (VERIFY WITH GOLDENS): For any renderer / dialog / layout change, add or refresh ' +
    'a golden snapshot in cpp_migration/tests/golden/<id>.txt. ' +
    'Use UPDATE_GOLDENS=1 once after the first build to write the snapshot. ' +
    'Do NOT ship renderer changes without at least one golden test that would break on drift.',
  'RULE #3 (FTXUI LIFETIME): FTXUI Component.Render() then dropping the Component = UAF. ' +
    'Wrap in a Node subclass that owns the Component (see fix pattern at commit bd507bc / ' +
    'CompEl in permissions_components.cppm). `el | [c](e){return e;}` does NOT keep c alive.',
  'RULE #4 (NO CONSTANT-RATE TICKERS): FTXUI idle frames cause cursor flicker. ' +
    'Rerender on state change ONLY. Park the native cursor at the last non-motion position. ' +
    '(Fix pattern at commit c01433a.)',
  'RULE #5 (PALETTE-DRIVEN COLORS): Never hardcode RGB. Always route through ' +
    'theme.color_for(Role::X). If a TS color has no matching Role/Palette field, ADD it ' +
    'to Palette + Role enum first (as done for Permission in commit 8e2b962).',
  'RULE #6 (SLOC BUDGET): A single .cppm module must not exceed ~12K effective lines. ' +
    'Clang has a ~2GB source-location budget; app.cppm blew it historically. Split into ' +
    'thin interface + per-aggregator .cpp impl units if >8K. (Fix pattern in memory.)',
  'RULE #7 (FTXUI v5 API): Use component->Render() — the legacy OnRender override was ' +
    'renamed. 27 sites corrected in commit of M7 Task#125.',
  'RULE #8 (COMMIT GRANULARITY): Edit code by module; leave golden .txt changes as a ' +
    'SEPARATE commit group. CMake changes go in their own commit if they add new module ' +
    'targets; otherwise inline with the user. Never mix golden data + code in one commit.',
  'RULE #9 (ENGLISH ONLY in docs/comments — Chinese in user chat only).',
  'RULE #10 (BUMP CMakeLists.txt IF you add a new .cppm / .cpp file. Check both build ' +
    'presets (Debug + Release). CI runs only on macos-14 so a Linux-only break is still bad ' +
    'even if CI is green.',
].join('\n');

const REPO_ROOT = '/Users/bytedance/Develop/CC-REPL';
const CPP_ROOT = REPO_ROOT + '/cpp_migration';

const results = [];
for (const entry of toImplement) {
  if (budget.total && budget.remaining() < 60_000) {
    log(`Budget remaining ${budget.remaining()} < 60k — stop loop, keep unimplemented ranked list.`);
    break;
  }

  const gap = entry.gap;
  phase('Implement');

  log(`── Implementing ${gap.id}  ${gap.title} ──`);
  log(`  TS:  ${gap.ts_path}\n  CPP: ${gap.cpp_path}\n  ` +
      `Est. ${gap.fix_estimate_lines} LOC, ${entry.composite_hours}h`);

  // --- Implement agent ---
  const impl = await agent(
    `You are the TS→CPP faithful-port IMPLEMENTOR. Output ONLY an IMPL_REPORT_SCHEMA object, ` +
    `then STOP. (Your free-text reasoning before the final JSON is fine but the final message ` +
    `must be the JSON matching the schema.)\n\n` +
    `GROUND RULES (read these FIRST; they are binding):\n${GROUND_RULES}\n\n` +
    `TARGET GAP:\n${JSON.stringify(gap, null, 2)}\n\n` +
    `RECIPE:\n` +
    `1. READ the TS reference at ${REPO_ROOT}/${gap.ts_path}. Understand the exact DOM, ` +
    `props, color tokens, padding, accessibility behaviour.\n` +
    `2. READ the CPP target at ${CPP_ROOT}/${gap.cpp_path}. Understand current divergence.\n` +
    `3. READ any existing tests under cpp_migration/tests/ that cover this component.\n` +
    `4. EDIT files under cpp_migration/src/ only. Do not touch TS src/. ` +
    `   For layout/renderer changes, add or refresh a golden .txt file under ` +
    `cpp_migration/tests/golden/ — but WRITE THE TEST FIRST (the test name must be meaningful; ` +
    `use the gap id as the golden basename if nothing fits better).\n` +
    `5. If a new Palette/Role token is needed, modify design_tokens.cppm (Palette + Role enum ` +
    `+ token_by_role) + theme_provider.cppm resolve_color.\n` +
    `6. If a new module is introduced, REGISTER the target in cpp_migration/src/CMakeLists.txt ` +
    `AND add a // TS REF comment.\n` +
    `7. Keep code comments in ENGLISH only. Add file-level TS REF lines.\n` +
    `8. Do NOT run cmake/ctest in this agent — a subsequent VERIFY agent does that.\n\n` +
    `FINAL DELIVERABLE: valid IMPL_REPORT_SCHEMA with accurate file lists + line counts.`,
    {
      label: `impl:${gap.id}`,
      phase: 'Implement',
      schema: IMPL_REPORT_SCHEMA,
      effort: gap.fix_estimate_lines > 500 ? 'xhigh' :
              gap.fix_estimate_lines > 200 ? 'high' : 'medium',
    }
  );

  // --- Verify agent (build + ctest) ---
  phase('Verify');
  log(`  Verifying build + ctest for ${gap.id}…`);

  const verify = await agent(
    `You are the VERIFY agent for gap ${gap.id}. Run the build + scoped ctest and return ` +
    `ONLY a VERIFY_REPORT_SCHEMA object.\n\n` +
    `STEPS (run ALL of them, in order, using Bash tool):\n` +
    `1. cd ${CPP_ROOT}\n` +
    `2. Incremental build RELEASE: cmake --build build/release -j 8 2>&1 | tee /tmp/build_${gap.id}.log\n` +
    `3. Count warnings: grep -c "warning:" /tmp/build_${gap.id}.log (return 0 if none)\n` +
    `4. Also run DEBUG build if release passed: cmake --build build/debug -j 8 >/dev/null 2>&1 ` +
    `   (just check RC; use /tmp/builddbg_${gap.id}.log for warnings).\n` +
    `5. Design a scoped ctest regex (pass via ctest -R). Strategy:\n` +
    `     - If any golden files were touched: include their test basenames.\n` +
    `     - If the gap touches subsystem X (UI-Messages / UI-Dialogs / etc.), match those test suites.\n` +
    `     - As a safety net, always include "DialogFrame|DialogRenderer|VirtualList|LogoV2|PromptDialog"` +
    `       plus any test family named after the CPP module file.\n` +
    `6. First PASS: ctest --test-dir build/release -j 8 -R <regex> --output-on-failure --test-timeout 30 2>&1 | tee /tmp/ctest_${gap.id}.log\n` +
    `7. IF there were golden failures, run UPDATE_GOLDENS:\n` +
    `       cd ${CPP_ROOT}/build/release && UPDATE_GOLDENS=1 ctest -j 8 -R <same-regex> --test-timeout 30\n` +
    `   THEN rebuild release just the affected test targets, THEN re-run ctest (step 6) clean.\n` +
    `8. Parse the LAST ctest run's total/pass/skip/fail counts from ctest stdout or from the ` +
    `   LastTest.log lines prefixed with "The following tests FAILED:".\n` +
    `9. Compute NEW failures: any failed test whose name is NOT in this PREEXISTING_FAILS set:\n` +
    `   ${JSON.stringify([...PREEXISTING_FAILS])}\n` +
    `10. Build VERIFY_REPORT_SCHEMA:\n` +
    `    - build_rc = last build RC. Treat warning_count = total from both presets.\n` +
    `    - ctest_scope = the actual regex you used.\n` +
    `    - ctest_* counts = from the LAST ctest run (post golden refresh, if any).\n` +
    `    - ctest_new_fails = array of test names from step 9.\n` +
    `    - golden_update_needed = true ONLY if step 7 was run AND it resolved golden mismatches.\n` +
    `    - verdict:\n` +
    `        PASS = build_rc==0 && ctest_new_fails.length==0\n` +
    `        BUILD_FAIL = build_rc != 0\n` +
    `        TEST_REGRESSION = build ok but ctest_new_fails non-empty\n` +
    `        GOLDEN_REFRESH_NEEDED = golden mismatches remain after step 7 retry\n` +
    `        UNKNOWN = ctest failed to produce counts`,
    {
      label: `verify:${gap.id}`,
      phase: 'Verify',
      schema: VERIFY_REPORT_SCHEMA,
      effort: 'high',
    }
  );

  // --- adversarial verdict: if TEST_REGRESSION, spawn a dedicated debugger agent ---
  let fixups = null;
  if (verify && verify.verdict === 'TEST_REGRESSION' && verify.ctest_new_fails.length > 0) {
    phase('Verify');
    log(`  Regression detected on ${verify.ctest_new_fails.join(', ')} — dispatching debugger.`);
    fixups = await agent(
      `DEBUG the following test regressions from gap ${gap.id}: ` +
      verify.ctest_new_fails.join(', ') + `\n` +
      `Read /tmp/ctest_${gap.id}.log tail, read failing test source, read related implementation.\n` +
      `Fix the underlying bug (do NOT downgrade test sensitivity).\n` +
      `Re-run VERIFY steps 2–10 until ctest_new_fails=[].\n` +
      `Return VERIFY_REPORT_SCHEMA for the FINAL state. If unable to fix within 3 iterations, ` +
      `return the final failing report but set verdict=TEST_REGRESSION and explain in a free-text ` +
      `note the blocker (but first check the PREEXISTING_FAILS set — regressions already there ` +
      `should be reclassified as PASS).\n` +
      GROUND_RULES,
      {
        label: `debug:${gap.id}`,
        phase: 'Verify',
        schema: VERIFY_REPORT_SCHEMA,
        effort: 'xhigh',
      }
    );
  }

  const finalVerify = fixups || verify;

  results.push({
    gap_id: gap.id,
    gap_title: gap.title,
    judgement: {
      priority: entry.composite_priority,
      risk: entry.composite_risk,
      hours: entry.composite_hours,
    },
    implementation: impl,
    verification: finalVerify,
    verdict_ok: finalVerify && finalVerify.verdict === 'PASS',
  });
}

// ==================================================================
// PHASE 5 — Loop: (optional) commit plan + return summary
// ==================================================================
phase('Loop');

const passed = results.filter(r => r.verdict_ok);
const failed = results.filter(r => !r.verdict_ok);

log(`Passed implementations: ${passed.length}/${results.length}`);
if (failed.length) {
  log(`  Failed / pending:\n` +
      failed.map(r => `    - ${r.gap_id} ${r.gap_title} → ${r.verification ? r.verification.verdict : 'no_verify'}`).join('\n'));
}

// ---- Produce commit plan if the caller asked for auto-commits ----
let commitPlan = null;
if (passed.length && (args && args.commit_mode === 'auto')) {
  commitPlan = await agent(
    `Inspect git status at ${REPO_ROOT}. Group the CPP changes (files under cpp_migration/ ` +
    `only; ignore TS src/ and .claude/ worktrees) into ATOMIC conventional commits following ` +
    `RULE #8 (golden data separate from code; CMake module-additions separate). Return a ` +
    `COMMIT_PLAN_SCHEMA.\n\n` +
    `Conventions:\n` +
    `  • feat(cpp-migration,SUBSYS):TITLE        — new modules, capabilities\n` +
    `  • fix(cpp-migration,LABEL):TITLE          — bug fixes, alignment tweaks\n` +
    `  • test(cpp-migration,LABEL):TITLE         — golden refreshes, new test harness only\n` +
    `  • build(cpp-migration):TITLE              — CMake / presets only\n` +
    `LABELs mirror the gap id / milestone (e.g. R7, P0-2, M7).\n\n` +
    `Do NOT actually run git commit. Produce the plan only.`,
    {
      label: 'commit-plan',
      phase: 'Loop',
      schema: COMMIT_PLAN_SCHEMA,
      effort: 'low',
    }
  );

  // ---- actually apply the plan: git add per commit, commit, then push only if args.push ----
  if (commitPlan && Array.isArray(commitPlan.commits)) {
    for (const c of commitPlan.commits) {
      const rc = await agent(
        `Shell commands ONLY. cd ${REPO_ROOT}. For commit subject "${c.subject}":\n` +
        `  1. git add ${c.files.map(f => JSON.stringify(f)).join(' ')}\n` +
        `  2. git commit -m ${JSON.stringify(c.subject + (c.body ? '\n\n' + c.body : ''))} --no-verify\n` +
        `Return the stdout. Do NOT push.`,
        {
          label: `commit:${c.subject.slice(0, 30)}`,
          phase: 'Loop',
          effort: 'low',
          schema: { type: 'object', required: ['ok'], properties: { ok: { type: 'boolean' } } },
        }
      );
      log(`  committed ${c.subject} ok=${JSON.stringify(rc)}`);
    }
    if (args && args.push) {
      await agent(`cd ${REPO_ROOT} && git push origin HEAD 2>&1 | tail -5`, {
        label: 'push', phase: 'Loop', effort: 'low',
        schema: { type: 'object', required: ['ok'], properties: { ok: { type: 'boolean' } } },
      });
    }
  }
}

// ==================================================================
// Final deliverable
// ==================================================================
return {
  scope,
  total_discovered: shortlist.length,
  total_judged: ranked.length,
  attempted: results.map(r => ({
    gap_id: r.gap_id,
    title: r.gap_title,
    priority: r.judgement.priority,
    risk: r.judgement.risk,
    verdict: r.verification ? r.verification.verdict : 'NO_VERIFY',
    new_failures: r.verification ? r.verification.ctest_new_fails : [],
  })),
  passed_count: passed.length,
  failed_count: failed.length,
  commit_plan: commitPlan,
  pushed: !!(args && args.push && passed.length),
  // keep the rest of the ranked list handy for the NEXT invocation of this workflow
  remaining_ranked: ranked
    .slice(toImplement.length)
    .map(r => ({
      id: r.gap.id, title: r.gap.title, severity: r.gap.severity,
      subsystem: r.gap.subsystem, ts_path: r.gap.ts_path, cpp_path: r.gap.cpp_path,
      priority: r.composite_priority, doable: r.composite_doable, risk: r.composite_risk,
      hours: r.composite_hours, prerequisites: r.composite_prerequisites,
    })),
  budget_spent_tokens: budget.spent(),
  budget_remaining_tokens: budget.remaining(),
};
