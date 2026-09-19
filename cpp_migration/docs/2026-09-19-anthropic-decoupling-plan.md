# Anthropic Decoupling — Executable Removal Plan (Phase A output)

> **Status:** Phase A (audit) COMPLETE · Phase B (delete) PARTIAL · Phase C
> (backend seam) COMPLETE · Phase D (rename to Loom) NOT STARTED.
> **Goal:** make CC-REPL a personal, Anthropic-independent project.
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

## 7. Remaining open question

**Rename paths (`~/.claude` → `~/.loom`)?** Breaks existing user skill dirs /
memory / settings. Deferred to the rename phase (§4); needs its own decision
because it is user-data-migrating, not just cosmetic.

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
`CC_REPL_WIRE_API` (env) selects the backend; unset ⇒ Anthropic, preserving
existing behaviour exactly. An OpenAI-compatible endpoint (llama.cpp, vLLM,
Ollama, OpenRouter, …) now runs the same agent loop, tools, permissions and UI.
Tests: 1655 → 1685 (24 backend unit tests + 6 engine↔seam integration tests in
`tests/test_tools.cpp` `WireSeam.*`).

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
