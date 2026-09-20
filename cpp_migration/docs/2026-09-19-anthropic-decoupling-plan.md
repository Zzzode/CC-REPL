# Anthropic Decoupling — Executable Removal Plan (Phase A output)

> **Status:** Phase A (audit) COMPLETE · Phase B (delete) COMPLETE · Phase C
> (backend seam) COMPLETE · Phase D (rename to Loom) COMPLETE, including the
> user-data cascade (§7) · Phase E (owner decisions of 2026-09-20) COMPLETE, §9
> · Phase F (pure-harness removal, 2026-09-21) COMPLETE, §10.
> **Goal:** make CC-REPL (now **Loom**) a personal, Anthropic-independent project.
> **Method:** 6 parallel read-only audits over the C++ tree; every load-bearing
> claim independently re-verified by the primary agent (2 audit claims were
> corrected — see §5).
> **Verification rule used throughout:** production importers decide, not file
> count and not the presence of the word "Anthropic". "Looks Anthropic but is a
> generic harness capability" ⇒ KEEP.

## 0. Headline findings

1. **Coupling is concentrated, not diffuse.** ~40 files are genuinely
   Anthropic-bound out of 1200+. The rest is naming.
2. **The live API path is NOT `services/api/client.cppm`.** `QueryEngine`
   (`query/query_engine.cppm`) owns its own `httplib::Client`, its own request
   serializer and its own response parser; the REPL/server/CLI all run through
   it. `AnthropicClient` has ~1 live call site. **Any backend swap edits
   `query_engine.cppm` first.**
3. **A large amount of Anthropic-bound code is already dead** (zero production
   importers). Whole dead islands: most of `src/bridge/`, the analytics
   cluster, several billing services. Deleting dead code is zero-risk and
   should come first.
4. **Real vendor credentials are committed** and unreferenced
   (`constants/keys.cppm` GrowthBook SDK keys; `analytics/datadog.cppm` client
   token). Highest-priority hygiene item.
5. **The highest-value capability we must NOT break** is computer-use: its
   engine has exactly ONE Anthropic mention (a comment). Only the tool-shape
   emission is Claude-API-specific.

## 1. DELETE — zero production importers (safe, do first)

### 1.1 Dead bridge island (14+ files)
`src/bridge/`: `bridge_debug`, `bridge_types`, `capacity_wake`, `pointer`,
`poll_config`, `poll_config_defaults`, `status_util`, `trusted_device`,
`inbound_attachments`, `envless_config`, `session_runner`, `bridge`,
`bridge_enabled`, plus the dead chains `bridge_main`, `init`, `ui`,
`inbound_messages`.
> Verified: the modules that DO have importers (`capacity_wake`, `poll_config`,
> `envless_config`, `bridge`, `bridge_enabled`, `init`, `ui`,
> `inbound_messages`) are imported ONLY by other dead bridge files
> (`bridge_main`, `core`, `init`, `session_runner`, `bridge`). Closed island.
> **KEEP in this dir:** `api.cppm` + `work_secret.cppm` (the only live code,
> reached via `daemon/daemon_server.cppm:40-41`), `messages.cppm`,
> `transport.cppm` (abstraction, but its WebSocket impl has no TLS),
> `bridge_messaging.cppm`, `session_id_compat.cppm`, `flush_gate.cppm`,
> `debug_utils.cppm`, `jwt_utils.cppm` (`base64url_decode` only), and the
> `TokenRefreshScheduler` half of `core.cppm`.

### 1.2 Analytics / telemetry cluster
`services/analytics/*` (analytics, config, metadata, sink, sink_killswitch,
datadog, first_party_event_logger, index), `types/experiment_event.cppm`,
`types/internal_event.cppm`, `types/auth.cppm`, `utils/telemetry.cppm`,
`utils/telemetry_exporters.cppm`, `services/telemetry/telemetry.cppm`,
`services/api/metrics_opt_out.cppm`, `services/api/grove.cppm`,
`services/api/referral.cppm`, `services/api/claude_api.cppm`,
`constants/keys.cppm`, `constants/github_app.cppm`.
> Note: `datadog.cppm` + `constants/keys.cppm` carry live credentials.

### 1.3 Dead billing / quota services
`services/api/ultrareview_quota.cppm`, `services/api/overage_credit.cppm`,
`services/api/first_token_date.cppm`, `services/api/claude_ai_limits.cppm`.

### 1.4 Dead auth / misc
`services/oauth/oauth_profile.cppm` (fake profile generator),
`cli/handlers/auth.cppm` (2nd login impl, conflicting credential schema),
`bootstrap/setup.cppm`, `screens/doctor_screen.cppm` (stub doctor),
`ui/components/auth_flows.cppm`, `utils/auth_portable.cppm`,
`utils/session_url.cppm`, `hooks/claude_code_hint_recommendation.cppm`,
`hooks/update_notification.cppm` (live call to
`api.github.com/repos/anthropics/claude-code`), `skills/bundled/claude_api.cppm`,
`ui/logo/logo_welcome.cppm`, `ui/dialogs/teleport_dialogs.cppm` (orphan),
`hooks/teleport_resume.cppm`, `commands/teleport.cppm`.

### 1.5 Services/rate_limit pre-existing orphans
`messages.cppm`, `mock.cppm` (verify first — `rate_limit.cppm` and
`claude_ai_limits.cppm` have tests-only importers).

## 2. DELETE — dead once §1 lands (ordering matters)

| Item | Depends on |
|---|---|
| `ui/dialogs/install_slack_app_wizard.cppm`, `ui/dialogs/desktop_upsell.cppm` | registry allowlist edits |
| `commands/{mock_limits,reset_limits,extra_usage}.cppm` | allowlist + init_e edits |
| `services/rate_limit/claude_ai_limits_hook.cppm` | the two commands above |
| `claudeai_proxy` enum member (`ui/dialogs/mcp_dialogs.cppm:72`) | — |

**Removal point for commands is the allowlist** `commands/command_registry.cppm:57-85`,
NOT the registration files. All three registration mechanisms must be edited
together.

## 3. REFACTOR — generic value, Anthropic-shaped

| Target | Work |
|---|---|
| `query/query_engine.cppm` | **The main event.** Carve the wire format into a seam: `build_request_body` / `content_to_json` / `append_message_to_json` / `add_beta_headers` / `api_messages_endpoint` + `parse_api_response` / `parse_content_block`. Emit computer-use as `{type:"function", input_schema}` (schema already exists, just never sent) behind a config switch. |
| `utils/model/providers.cppm` | Unify THREE parallel `Provider` enums (`models.cppm:17`, `providers.cppm:11`, `provider_selector.cppm:68`) into one; add a non-Claude branch. |
| `services/auth/provider_selector.cppm` | KEEP the abstraction (Bedrock SigV4 / Vertex ADC / Foundry Entra are provider-native); remove only the `FirstParty` branch and rework `client.cppm`'s gate on it. |
| `services/oauth/client.cppm` | Already vendor-clean (zero Anthropic hits). Strip defaults: keychain service name, redirect port. |
| `commands/login.cppm` | Keep credential write (0600), API-key path; drop the claude.ai OAuth branch + scopes. |
| `commands/logout.cppm` | **Bug:** hardcoded client-id literal (`:142`) that must not drift from login's. |
| `constants/oauth.cppm`, `constants/product.cppm`, `constants/constants.cppm` | Strip vendor URLs/IDs/scopes; keep the struct shapes. |
| `utils/teleport_utils.cppm` | Split: keep the generic git-bundle chain + helpers; delete the claude.ai Sessions/Environment/Files API half. **Already env-overridable** (`CC_REPL_REMOTE_API_BASE_URL:825-827`). |
| `ui/dialogs/trust_utils.cppm` | 9 vendor hosts in `kSafeHosts` (`:461-480`). **Security surface** — duplicated in `commands/plugin/plugin_trust_text.cppm:70-81`. |
| `ui/screens/doctor_screen.cppm`, `commands/doctor.cppm` | Re-point `network_endpoint`; 3 divergent "doctor" implementations should collapse to one. |
| `skills/schedule_remote_agents.cppm` | Keep the NL→cron parser; delete the hosted-scheduler front-end. |
| `commands/review/ultrareview.cppm` | Keep the multi-round review plan generator; delete the billing gate. |
| Tools/hooks/UI cosmetic passes | `built_in_agents.cppm` + `agent_runtime.cppm` carry **duplicated** identity prompts — rebrand both or dedupe first. |

## 4. Cosmetic rebrand (do LAST — test/golden locked)

Direct brand-string assertions: `tests/test_ui_runtime.cpp` (10 sites incl. 2
E2E gates), `tests/test_state.cpp:1175`, `tests/test_commands.cpp:667`.
Goldens: `welcome_header.txt`, `logov2_render_modes_missing_*.txt`,
`trust_dialog_workspace_low.txt`.
**Art invariants:** `logo_v2.cppm:76` (`kWelcomeV2FixedWidth = 58`, enforced
`:1035`) and `design_system/logo.cppm:221/:332` — a different-length caption
desyncs hand-padded ASCII art.

**Cross-module contracts — rename both sides or neither:**
- `CLAUDE_HOOK_*` (`hooks/shell_hooks.cppm:143-166` ↔ `tools/agent_sub_utils.cppm:1792-1808`)
- `${CLAUDE_SKILL_DIR}` / `${CLAUDE_SESSION_ID}` (`tools/skill_tool.cppm:747` ↔ `skills/load_skills_dir.cppm:240`)
- `"yes-claude-folder"` ↔ `"claude_folder"` (`permissions/permission_file_write.cppm:404` ↔ `repl_screen.cppm:861`)
- Theme keys in `theme_provider.cppm:254-334` — keep Anthropic names as aliases (user theme JSON)
- `"claude"` @-mention token (`autocomplete_sources_impl.cpp:295-301`) — user-typed
- **`agent_runtime.cppm:3987` string-matches an error produced elsewhere** (`"No Claude.ai OAuth access token found"`) — renaming the producer breaks remote-agent polling silently.

**Follow the existing correct pattern:** `app.cppm:1128-1132` reads `CC_REPL_*`
first with `CLAUDE_CODE_*` as fallback.

## 5. Audit claims I verified and corrected

| Claim | Verdict |
|---|---|
| "The entire command registry has no production caller" | **WRONG.** `main.cpp:2074` → `cc_ui_run_app_bridge` → `ui/app.cppm:2185` (live TUI loop). Deleting commands DOES change user-visible behaviour. |
| "`ExtraUsageCommand` collides across namespaces" | **CONFIRMED** — defined in both `runtime_surface_commands.cppm:123` (macro) and `remote_commands.cppm:42`. Name-based deletion is unsafe. |
| "computer-use is generic; 1 Anthropic hit" | **CONFIRMED** — `computer_use.cppm:841` is a comment; `computer_20241022` appears in 6 places, only one of which emits. |
| "GrowthBook client is an env-var reader in a remote-SDK costume" | **CONFIRMED for `growthbook.cppm`** (no HTTP; values from env). But `references` to `api.anthropic.com` in the cluster are mostly string-only. |
| "committed live credentials" | **CONFIRMED** — `constants/keys.cppm:13-15` (3 GrowthBook SDK keys), `analytics/datadog.cppm:39` (Datadog token). Both zero-importers. |
| "bridge is a mostly-dead island" | **CONFIRMED** — importer graph closes inside dead files; live path is `api.cppm` + `work_secret.cppm` via `daemon_server`. |
| "3 parallel SSE implementations, one dead" | **CONFIRMED** — `services/api/sse_client.cppm` (639 L) has zero production importers. |

## 6. Owner decisions (2026-09-19)

1. **Model backends: TWO targets — OpenAI-compatible AND Anthropic-compatible.**
   "拿到 API 就可以用" — a user with either kind of endpoint should be able to
   point Loom at it and go. ⇒ §3 must produce a backend seam with (at least)
   two concrete implementations, not a generic plugin API. The
   Anthropic-compatible one keeps the existing wire format (so the current
   serializer becomes one implementation, not dead code); the
   OpenAI-compatible one is new (chat/completions + SSE `data:` frames,
   different tool-call shape, no thinking blocks).
2. **Local telemetry: KEEP.** `analytics/index.cppm` writes local NDJSON to
   `~/.local/state/<product>/analytics.ndjson`; no network. Everything that
   ships data to Anthropic/Datadog/GrowthBook is deleted.
3. **Local rate-limit display: KEEP and WIRE IN.** `claude_ai_limits_hook` is
   misnamed — it is generic 429/529 backoff with UI callbacks, currently
   reachable only from the dev `/mock-limits` and `/reset-limits` commands.
   Re-point it at the real request path (or unify with the engine's inline
   retry loop) so limit state is visible regardless of backend.
4. **Project name: LOOM.** Chosen for "weaving many agents/tools/memories into
   one fabric". Short, lowercase-typable as a command, product-viable.
   Rename is the LAST phase (§4) because it is test/golden-locked.

### Consequence for §3 (backend seam)

Because BOTH backends are required, the seam is:
- keep `RequestSerializer`/`ResponseParser`/`StreamEventParser` as the
  **Anthropic-compatible implementation**;
- add an OpenAI-compatible sibling (chat/completions, `choices[].delta`,
  `tool_calls[]`, `finish_reason`);
- make `QueryEngine`'s serializer/parser call through that seam instead of
  inlining one wire format;
- computer-use tool shape becomes config-driven: native `computer_20241022`
  for the Anthropic backend, ordinary `{type:"function", input_schema}` for
  the OpenAI-compatible one.

## 7. Path migration (resolved — see §7.1 and §7.2)

**Rename paths (`~/.claude` → `~/.loom`)?** Answered 2026-09-20: read through a
cascade, never write through it. The inventory of what the blanket rename moved
is §7.1; the decision and its implementation are §7.2.

### 7.1 What the rename actually did to paths (recorded after the fact)

The blanket rename moved six path families at once. Three of them are fine;
three are the open question.

**Correctly left alone — external contract, not our name to change:**

| Path | Why it must stay |
|---|---|
| `.claude-plugin/` (manifest dir, ~15 sites) | A plugin *format* specification. Third-party marketplaces ship this directory name; renaming it breaks every plugin that works today. The rename script masked it deliberately. |
| `mcp__claude-in-chrome__*` tool ids (~12 sites) | Tool names a *third-party MCP server* advertises. We match them; we do not mint them. |
| `ANTHROPIC_*`, `anthropic-version`, `anthropic-beta`, `computer_20241022`, model ids | Wire values — the whole point of the KEEP-AS-BACKEND class. |

**Moved by the rename, with a consequence worth stating:**

| Path family | Now resolves to | Consequence |
|---|---|---|
| Session/memory/skill/plugin data | `~/.loom/*` | A user's existing `~/.claude/CLAUDE.md`, skills and settings stop being read directly, but are still *read* through the cascade (§7.2). |
| `~/.config/loom/credentials.json` (Linux), XDG `loom/` | new | Old `~/.config/claude/` credentials are not found; login is re-run. Acceptable — it fails safe, does not send a stale token anywhere. |
| `~/.config/gcloud/application_default_credentials.json` | unchanged | Vendor-neutral Google path; correctly untouched. |

**Deliberately kept as a fallback (the pattern the plan called for):**

`CC_REPL_*` → the pre-rename spelling. Where the rename introduced a new
`LOOM_*` variable the old name is still read as a fallback
(`query_engine.cppm` `LOOM_WIRE_API`, `main.cpp`'s nine
`set_env_value_pair` sites, `app.cppm`'s config reads). One deliberate
exception: the `~/.cc-repl/skills` *directory* root in
`tools/skill_tool.cppm:238` is a second scan root, not a fallback — both
`.loom/skills` and `.cc-repl/skills` are scanned.

### 7.2 The user-data migration (DECIDED and IMPLEMENTED 2026-09-20)

Owner decision: **read** through a cascade, but never write outside our own
directory.

    config dir   $LOOM_CONFIG_DIR > ~/.loom > ~/.agents > ~/.claude
    memory file  LOOM.md > AGENTS.md > CLAUDE.md

Both cascades now live in exactly one module, `src/constants/paths.cppm`. Eight
walkers had each reimplemented the lookup with their own hardcoded filename —
query_engine, hooks/context, memdir/memory, memdir/paths,
utils/system_directories, config/settings, hooks/shell_hooks — so a change to
any one applied to one code path and not the others. All now delegate.
(`utils/system_directories.cppm` was deleted outright: zero importers, and its
`get_claude_config_dir` contradicted the cascade by resolving to XDG
`~/.config/loom` on Linux. Dead code that also disagrees with the policy is
worse than absent.)

Two points that were not obvious until implementation, both now pinned by
tests:

1. **Read and write are split.** `config_home_read()` follows the cascade;
   `config_home_write()` is `$LOOM_CONFIG_DIR` else `~/.loom`, never the
   cascade. Reading a legacy `~/.claude` is the intended behaviour; writing
   our `sessions/`, `plugins/` and state into it is not. `~/.claude` already
   has its own `sessions/`, so a user with only that directory would have had
   two tools' state interleaved in a directory neither of them controls.
   Verified on the real binary with a temp `HOME`: writes land in
   `~/.loom/sessions`, `~/.claude` is untouched.
2. **The memory-file cascade applies per directory, while walking up** — not
   "find any `LOOM.md` in the tree first". A `CLAUDE.md` beside the code beats
   a `LOOM.md` five levels up. The alternative lets a distant file of the
   preferred name override the file next to what is being edited. `/init`
   still creates `LOOM.md`, because new files use the preferred name.

See §9 for the full record of the three 2026-09-20 owner decisions.

## 8. Execution log

### Phase B — deletions (partial)

| commit | what |
|---|---|
| `9658d71` | dead analytics/telemetry/billing clusters + scattered dead files |
| `487960d` | Extra-Usage billing command; zero-importer orphans |
| `8847df3` | Slack app install flow |

Tests went 1671 → 1655 net across these (the count moved with the deltas, it is
not a pure subtraction: some deleted files carried tests, some did not).

### Phase C — backend seam (COMPLETE)

| commit | what |
|---|---|
| `a79e300` | `query/wire_protocol.cppm` — `WireApi`, `RequestInput`, `PreparedRequest`, `ParsedResponse`, `StreamDelta`, abstract `WireBackend` |
| `e87b0d2` | `query/wire_anthropic.cppm` + `query/wire_openai.cppm` — the two backends, 24 new tests |
| (this commit) | `query_engine.cppm` wired through the seam: `make_wire_backend()` + `build_wire_input()`; the old 198-line inline serializer deleted; three engine helpers the extraction orphaned removed |

**Result:** the engine no longer knows a wire format. `wire_api` (config) or
`LOOM_WIRE_API` (env, was `CC_REPL_WIRE_API`) selects the backend; unset ⇒
Anthropic, preserving
existing behaviour exactly. An OpenAI-compatible endpoint (llama.cpp, vLLM,
Ollama, OpenRouter, …) now runs the same agent loop, tools, permissions and UI.
Tests: 1655 → 1685 (24 backend unit tests + 6 engine↔seam integration tests in
`tests/test_tools.cpp` `WireSeam.*`).

### Phase D — rename to Loom (code-complete)

| commit | what |
|---|---|
| `8726e44` | the rename itself, plus the repairs for what it silently broke (395 files) |
| `b76b1d2` | title-case the product name in user-visible strings; `LOOM_WIRE_API` read rewritten plainly |
| `d215b9b` | §7 rewritten: the path inventory + the migration question, scoped but unimplemented at that point (implemented 2026-09-20, §9) |
| `59bda01` | two tests pinning the legacy env fallback (mutation-checked), and an RAII env guard |

**The rename's real hazard was not the renaming — it was that a blanket
replace MERGES things that were distinct.** Four sub-classes were found and
repaired, all of them silent at compile time:

1. **Merged name pairs.** `getenv("A")` falling back to `getenv("A")` — a
   fallback that can never fire because both legs became the same string. 11
   read sites, 9 write sites (`main.cpp`'s `set_env_value_pair`).
2. **Merged paths.** `skill_root_dirs()` had two *different* install
   locations collapse into one entry, so the same directory was scanned twice
   and every skill counted double. Same shape in
   `path_validation.cppm`: the write guard named only `.claude` /
   `.config/claude`, so `~/.cc-repl` — where credentials, settings and plugin
   trust actually live — was Bash-writable. **That one was a live security
   hole**, found by re-verifying the audit rather than by the classifier.
3. **Fabricated domains.** A rename applied to a hostname produces a domain
   that does not resolve and that nobody owns. All vendor URLs are now
   user-supplied (env) or absent, and the OAuth flow reports "no provider
   configured" instead of pointing at someone else's server.
4. **Fixture drift.** Goldens are written with the renamer's output but read
   with the original's, or vice versa; and a fixture renamed in one test but
   not its sibling stops exercising the branch it names.

**Two invariants the rename must not touch** (they are not our names to
change): `.claude-plugin/` is a plugin *format* spec that third-party
marketplaces ship, and `mcp__claude-in-chrome__*` are tool ids a third-party
MCP server advertises. Both were masked deliberately.

Tests: 1685 → 1689, debug and release both 100% at `-j1`.

### Phase A re-audit — corrections to this document

A second read-only sweep found several claims above are now stale or wrong.
Recorded here so the plan is not trusted past its evidence:

1. **§2 "three doctor implementations" is now TWO.** The stub was deleted in
   `487960d`. What remains is a real, intentional split, not redundancy:
   `commands/doctor.cppm` (491 L) is a thin CLI shim that returns
   `"UI:doctor"` for the no-arg case and text-renders for
   `--verbose`/`--fix`/`-i`; `ui/screens/doctor_screen.cppm` (1384 L) is the
   interactive screen. Collapsing them is a refactor with a UX decision
   attached, not a deletion. **The plan's "converge the three" item should be
   dropped.** One residual to fix: `screens/screens.cppm:201` defines a second,
   vestigial `DoctorScreen` class that name-collides and is imported only by
   `tests/test_utils.cpp`.
2. **Both `namespace oauth` blocks in `constants/constants.cppm` are dead** —
   every symbol in lines 143–150 and 509–526 resolves only inside that file.
   Safe to delete outright. In fact the module's live surface is three
   constants (`kVersion`, `kAppName`, `api_limits::kMaxTokensDefault`); sixteen
   of its namespaces have zero external references. Deleting the two `oauth`
   blocks is a hygiene fix, not the "duplicate namespace" problem §2 implied.
3. **`services/oauth/` is vendor-clean and LIVE** — parameterized, no
   hardcoded Anthropic endpoints. The Anthropic binding is in its *callers*:
   `commands/login.cppm:346-360` (real `client_id`, `claude.com/cai/oauth/*`)
   and `commands/logout.cppm:141,164`. Keep the library, delete the config.
   §2's "already vendor-clean" was right; the earlier framing was not.
4. **No GrowthBook/Datadog SDK or credential survives.** All ~30 hits are
   comments or local stubs. `constants/keys.cppm` and `analytics/datadog.cppm`
   (the §0.4 "committed credentials") are already gone. Nothing left to delete.
5. **New landmine found — a cross-module string coupling.**
   `tools/agent_runtime.cppm:3987` matches the error text produced at
   `utils/teleport_utils.cppm:616` (`"No Claude.ai OAuth access token found"`).
   Renaming that producer silently breaks remote-agent polling. Must be changed
   as a pair.
6. **Newly confirmed dead, deletable without a rename:**
   `constants/system.cppm` (zero importers despite being listed in
   `src/CMakeLists.txt:446`) and `utils/claudemd.cppm`. Both hold
   brand-bearing strings, so deleting beats rewriting.

**Open, unresolved by static analysis:** whether `commands/login.cppm`'s OAuth
branch is reachable in a default run, or whether the `ANTHROPIC_AUTH_TOKEN` /
`ANTHROPIC_API_KEY` short-circuits always win first — this decides whether the
real Anthropic OAuth endpoints are live or merely present. Needs a runtime
trace before Phase D touches `login.cppm`.

## 9. Phase E — owner decisions of 2026-09-20 (all implemented)

Three questions were put to the owner and answered. Recorded with the
reasoning, because in each case the answer was narrower than the obvious one.

**1. Legacy config is READ through a cascade, never written.**

    read:   $LOOM_CONFIG_DIR > ~/.loom > ~/.agents > ~/.claude
    write:  $LOOM_CONFIG_DIR > ~/.loom            (never the cascade)

The split is the substance of the decision. Reading another tool's config
directory is what the cascade is for; writing our `sessions/`, `plugins/` and
state into it is a different act, and `~/.claude` already has its own
`sessions/` for the two to collide in. A user with only `~/.claude` now gets
`~/.loom` created for our state, and theirs is left alone. Single
implementation in `src/constants/paths.cppm`; eight previous walkers
delegated to it.

**2. Memory files cascade per directory: LOOM.md > AGENTS.md > CLAUDE.md.**

Applied directory-by-directory while walking up, not "find any LOOM.md in the
tree first". Consequence: a `CLAUDE.md` next to the code beats a `LOOM.md`
five levels up. The alternative lets a distant file of the preferred name
override the file beside what is being edited, which is worse than the
branding inconsistency it would avoid. `/init` still creates `LOOM.md`
because new files use the preferred name.

**3. Local telemetry: a real writer, not just a decision to keep one.**

The module the plan named as "kept" was a dead stub with zero importers, so
this was owed work, not a preserved feature. Now
`src/services/analytics.cppm`: NDJSON appended to
`<XDG_STATE_HOME>/loom/analytics.ndjson`, no network path, and a test that
reads the module source and fails if an HTTP import appears (a runtime test
cannot observe "did not open a socket"). Wired into `QueryEngine`'s
constructor, which every entry point passes through, so it is populated in
normal use rather than being another writer nobody calls.

**Still open at the time of writing:** the `commands/login.cppm` OAuth
reachability question at the end of §8. Resolved 2026-09-21 by deletion rather
than analysis — see §10.

## 10. Phase F — pure-harness removal (2026-09-21)

Owner decision: the project is a **pure harness**. There is no site, no hosted
model, and no domain, so anything that exists to authenticate against, or reach,
a service we do not run is not a latent feature — it is a feature with no
possible backend. That reframes several items §3 and §8 had listed as
"refactor" or "strip the vendor bits" as deletions instead.

**Deleted (five commits).**

| Area | What went |
|---|---|
| Dead modules | Six modules registered in CMake with zero importers and zero symbol references (`mcp_auth_tool`, `auth_file_descriptor`, `api_key_verification`, `commands/mcp/xaa_idp`, `ui/dialogs/onboarding`, `services/mcp/normalization`). The last is worth noting: it shared a name with the TS `normalization.ts` while implementing an unrelated API, and did not contain the function it appeared to port. |
| Account login | `commands/{login,logout,oauth_refresh}.cppm`, `constants/oauth.cppm`, `services/oauth/client.cppm` (989 L, incl. KeychainStore), `utils/auth_utils.cppm`. This also answered §8's open question by removing both branches. |
| GitHub App | `/install-github-app` (2125 L, 12 steps, zero tests) — step 4 installed a GitHub App that does not exist, step 9 was the login flow, generated workflows referenced an action that does not exist. |
| Remote/teleport | `src/remote/**`, `utils/teleport_utils.cppm`, `tasks/remote_agent_task.cppm`, `tools/remote_trigger_tool.cppm`, `skills/schedule_remote_agents.cppm`, `commands/{remote_env,remote_setup}.cppm`, `ui/dialogs/remote_env_dialog.cppm`. |
| Fabricated domains | The `loom.ai` prefix test in `tool_deny_rules.cppm` (unreachable in practice, and the only thing exercising it was its own test), the fabricated GitHub App URL, the orphaned `constants::github_app` namespace, the "Loom.ai subscription" statusline text, and the `loom_ai_limits_hook` module name. |

**Deliberately KEPT — the line is "does it authenticate against *our* service".**

- **MCP OAuth** (`services/mcp/{auth,xaa,xaa_idp_login,oauth_port}`,
  `services/oauth/{auth_code_listener,crypto,types}`). This authorizes against a
  third-party MCP server *the user configured*. It is not our account. Note the
  989-line `services/oauth/client.cppm` was NOT shared with MCP: MCP uses only
  `auth_code_listener`, `crypto` and `types`. That check is what made the
  deletion safe, and it is the first thing to re-verify if this area is touched
  again.
- **Cloud-provider credentials** (`services/auth/`: SigV4, GCP ADC, Azure
  Entra). Open specifications, so a user can point Loom at their own Bedrock /
  Vertex / Foundry endpoint. §6.1's decision stands.
- **`ANTHROPIC_API_KEY` / `ANTHROPIC_AUTH_TOKEN`.** Still read, still reach the
  wire through the single decision point in `query/wire_anthropic.cppm` (Bearer
  if a token is set, else `x-api-key`). Carrying a user's credential to their
  endpoint is not an account system.

**Two couplings resolved by deletion rather than repair.** §8 and §4 both
flagged `teleport_utils.cppm`'s `"No Loom.ai OAuth access token found"` producer
and the `find()` match on it in `agent_runtime.cppm` — that match was the remote
poll loop's kill switch, so a rename on one side alone would have made the loop
retry forever. Both sides are gone. Likewise `tool_deny_rules.cppm`'s prefix
constant and its test.

**One rename artifact found and fixed rather than deleted.**
`agent_runtime.cppm`'s `remote_json_string_field` was used by *local* transcript
and sidechain parsing as well as the remote code, so deleting the remote half
would have taken a live helper with it. Renamed to `json_string_field`.

**Incidental dead code removed:** `render_welcome()` in `ui/layout/logo.cppm`
(zero callers, still shouting `LOOM`), and a vestigial `DoctorScreen` class in
`screens/screens.cppm` that name-collided with the live one in
`ui/screens/doctor_screen.cppm`.

Tests: 1724 → 1700. Debug and release both 100% at `-j1`. End-to-end verified
with a temp `HOME`: `loom --headless` runs, writes only
`<XDG_STATE_HOME>/loom/analytics.ndjson`, and creates no credentials file.

**Recorded but NOT acted on:** the C++ port uses `notifications/loom/channel`,
`loom/channel`, `loom/channel/permission` as MCP wire method names, whereas the
TS originals are `notifications/claude/channel` etc. These are not fabricated
domains — they are method-name namespaces a server advertises — but the rename
moved them off the values the TS speaks, so a real channel server would match
neither spelling. Out of scope here; it needs its own decision. (The channel
feature is currently disabled upstream at `is_channels_enabled()`, which returns
false, so nothing is broken today.)
