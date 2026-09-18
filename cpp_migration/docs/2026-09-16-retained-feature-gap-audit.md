# 2026-09-16 Retained-Feature Capability Gap Audit (bridge · teams · memory · computer-use)

> **Status update (2026-09-18):** most P0 gaps below are now CLOSED. See the
> **"Closure log"** section directly under TL;DR for what shipped; the
> historical findings are retained below as evidence.


> **Audit type:** POSITIVE completeness audit (find what is missing/incomplete in
> features the product KEEPS), not a pruning audit.
> **Scope decided by owner:** TUI stays; multi-agent teams (in-process + tmux +
> iTerm, UX reference: **herdr**) stay; local memory stays; computer-use stays;
> **bridge/remote-control stays**. Explicitly NOT wanted: voice, the MCPB
> plugin-packaged-MCP distribution mechanism (generic MCP client still wanted),
> cloud memory sync.
> **Method:** 4 parallel deep audits comparing TS reference (`src/`) against
> C++ (`cpp_migration/src/`), verifying real call sites (not file existence).
> **Recurring defect (critical to understand this report):** the C++ tree
> contains many modules that compile and have unit tests but have **ZERO
> production importers / are never invoked end-to-end**. "File exists" is not
> "feature works".

## TL;DR

| Domain | Surface impression | Verified reality |
|---|---|---|
| Generic MCP client (stdio/SSE/HTTP spawn, handshake, tools/list, call, images) | present | **WORKS** — real fork/exec, merge into model tools, screenshots returned via the explicit `mcp` tool |
| Computer-use (Anthropic native loop) | `computer_use.cppm` 853 LOC, registered | **FAKE** — registered as an ordinary function tool; native `computer_20241022` schema never emitted; **non-screenshot actions return no screenshot** so the model is blind after acting; permission panel orphaned; capture macOS-only |
| Teams — in-process | `agent_tool.cppm`, team_create | **CORE WORKS** — real `std::thread` per teammate, inbox drain, cancel, native-store message routing |
| Teams — tmux/iTerm panes (herdr-style) | `swarm_backends.cppm` 1447 LOC shells out to tmux | **HALF** — panes launch and tmux commands are real, BUT tmux env is never captured (detection dead-always-false), spawned pane REPL never reads its mailbox (ignores instructions), no permission sync, no capture-pane watchability, a second dead toy backend coexists |
| Memory — local | `memdir/`, `services/memory/`, remember skill | **NON-FUNCTIONAL end-to-end** — the memory prompt is never injected into the system prompt (0 importers), the canonical auto-memory path `~/.claude/projects/<root>/memory/` does not exist, 5 divergent invented stores, extract/session-memory built but never invoked, remember skill points at a nonexistent JSON file |
| Bridge — v1 work daemon (`--bridge-daemon`, poll+fork+wss worker) | present | **WORKS** (SSE worker degraded: no TLS in `sse_transport`) |
| Bridge — v2 interactive remote-control (phone/web drives the REPL) | `core.cppm`/`init.cppm` present | **STUB, zero callers** — fake session id / fake worker JWT, ReplV2Transport never opens SSE, CCR v2 worker protocol not implemented, no trusted-device enrollment, nothing invokes `init_env_less_bridge_core`; **cannot pair a device or drive the agent remotely today** |

Net: the **agent loop, core tools, in-process sub-agents, generic MCP, and the
v1 bridge worker are real and usable**. Everything that makes a *watchable
multi-pane team*, a *self-improving memory*, a *native computer-control loop*,
and a *phone-pairable remote REPL* is currently scaffolding that needs wiring.

## Closure log (2026-09-18)

| Gap | Status | Evidence |
|---|---|---|
| Computer-use #1 — emit native `computer_20241022` tool | **CLOSED** | `query_engine.cppm` add_tool emits `{type:"computer_20241022", name:"computer", display_width_px/height_px/number}` for internal `computer_use` def; geometry env-overridable (`CC_REPL_COMPUTER_DISPLAY_*`). Tests `NativeComputerToolEmitsComputer20241022Schema`. |
| Computer-use #2 — screenshot after every action | **CLOSED** | `ComputerUseManager::dispatch_input` → `with_post_action_frame` attaches a full-screen frame to every successful input action; local adapter still macOS-only by design. |
| Computer-use #3 — bind to `computer-use` MCP server + preserve raw-name fallback images | **CLOSED** | `execute_computer_use` forwards verbatim to a connected ready `computer-use` server (checked BEFORE local action parsing, fail-closed); `mcp_result_to_tool_result` now used by the `mcp` wrapper and all three raw-name fallback sites (main ×2, server_routes), preserving image blocks. Tests `ComputerActionRoutesToComputerUseMcpServer`, `ResultConversionPreservesScreenshotImage`. |
| Computer-use #4 — carry MCP input schemas verbatim | **CLOSED** | `parse_list_tools_result` now extracts `inputSchema`; flows service `McpTool.input_schema_json` → native `McpToolInfo` → `collect_mcp_input_schemas()` → `QueryEngineConfig::mcp_input_schema_provider` → request `input_schema` emitted verbatim (nested shapes, `$ref`, vendor keys). Test `VerbatimNestedSchemaEmitted`. |
| Computer-use #5 — reject `image/rgba` | **CLOSED** | media type restricted to png/jpeg/webp/gif; rgba/unknown defaults to png (`runtime_registry.cppm`). |
| Teams — tmux env, mailbox consumption, permission sync, watchability | **CLOSED** | commits c295da8, e0e9520, f5922dc: tmux env capture, pane inbox drain + initial-task delivery, balanced splits, leader-exit cleanup, filesystem permission request/response with fail-closed timeout, live teammates projection + pane observer UI, reconnection. |
| Memory — canonical path + prompt injection | **CLOSED** | commit fdb0653. |
| Memory — LLM auto-extraction | **CLOSED** | commit 84019d2; opt-in `CC_REPL_ENABLE_MEMORY_EXTRACTION=1`, member `std::jthread` (no UAF), sub-engine recursion suppressed. |

Still open (ranked): computer-use #6 permission-panel wiring; cross-process
flock + always-allow persistence + diff-bearing approval dialog in teams;
iTerm2 backend; session `summary.md` compaction; **bridge v2 pairing** (largest).

### Teams hardening addendum (2026-09-18, follow-up)

- **Cross-process flock** — CLOSED. Every inbox read-modify-write
  (write_to_mailbox, mark_all_read, permission response removal) now takes an
  exclusive `flock` on `<inbox>.lock` in addition to the in-process mutex,
  serializing leader ↔ separate pane processes. Verified with a two-child
  fork concurrency test (`CrossProcessFlockSerializesInboxWrites`).
- **Always-allow persistence to worker** — CLOSED. AlwaysAllow now attaches
  an SDK-shaped `permission_updates` addRules update to the success envelope;
  the worker persists whole-tool grants to
  `<team>/permissions/worker-allow-<agent>.json` and short-circuits matching
  future permission checks without a mailbox round-trip (survives pane
  restart; content-scoped rules intentionally not auto-matched). Test
  `AlwaysAllowUpdatesPersistAndGrant`.
- **Approval dialog shows tool input/diff** — CLOSED.
  `format_permission_request_input` renders Edit inputs as a `-`/`+` diff
  (with file_path), other tools as pretty JSON, size-capped. Test
  `PermissionInputFormatting`.

Still open after this addendum: iTerm2 backend (macOS); session
`summary.md` compaction; **bridge v2 pairing** (largest).

### Session compaction summary (2026-09-18, follow-up 2)

- **Session memory `summary.md`** — CLOSED. Compaction appends its summary
  to `<config_home>/projects/<sanitized-cwd>/<sessionId>/session-memory/
  summary.md` (TS getSessionMemoryPath); the engine injects the accumulated
  file as a `<context name="session-memory">` block so resumed sessions
  (`--resume`/`--continue`, new `QueryEngineConfig::session_id_override`)
  retain pre-compaction knowledge. Test
  `CompactionPersistsSessionSummaryForResumedSession`.

Still open after this addendum: iTerm2 backend (macOS);
**bridge v2 pairing** (largest).

### Computer-use permission panel (2026-09-18, follow-up 3)

- **Computer-use #6 permission panel** — CLOSED. `permission_computer_use`
  is no longer orphaned: the live permission callback detects computer
  actions (`options_from_tool_input` on the native `action`/`coordinate`/
  `text` shape, plus local aliases) and mounts a dedicated
  `DetailComputerUse` block in the shared single-prompt panel — action
  label, target app, coordinates, typed text, and the
  "first computer use in this session" warning. Non-computer inputs fall
  through to the generic panel. Tests `ComputerUseDetailRendersActionAndTarget`,
  `ComputerUseInputParsingRecognizesActions`.

Still open after this addendum: iTerm2 backend (macOS);
**bridge v2 pairing** (largest).


---

## 1. Computer-use — gaps to close the see→act loop over MCP

Generic MCP is complete, so computer-use should be built as the **standard
Anthropic computer tool bound to a computer-use MCP server**, not by porting the
ant napi/Swift wrapper (`@ant/computer-use-*`, macOS host adapter).

Ranked work:
1. **Emit the native computer tool.** Extend `core::ToolDefinition`
   (`tools/tool.cppm:203`) with an optional tool kind + display geometry, and
   teach the serializer (`query/query_engine.cppm:2028-2038`, mirror
   `services/api/client.cppm:348-364`) to emit
   `{type:"computer_20241022", name:"computer", display_width_px,
   display_height_px, display_number}`. Today only name/description/input_schema
   are serializable — structurally impossible to send the native tool, so the
   model never enters computer-use mode. **Highest leverage.**
2. **Return a screenshot after EVERY action**, not just the explicit screenshot
   action. `computer_use.cppm:840-845` / `runtime_registry.cppm:1531-1547` end
   with `ActionResult::ok()` and attach no frame. After move/click/drag/type/
   hotkey/scroll, capture and attach the post-action frame. This is the single
   most important behavioral fix.
3. **Bind the native tool to the configured `computer-use` MCP server** and
   preserve images on the raw-name MCP fallback (`main.cpp:1901-1907` flattens
   to one text block; the explicit `mcp` branch at
   `runtime_registry.cppm:1846-1864` already preserves images). Port
   `isComputerUseMCPServer`.
4. Carry MCP **input schemas** through discovery (`mcp_tool.cppm:184-185`
   drops `input_schema_json`; `runtime_registry.cppm:2769` emits empty schema).
5. Display geometry / logical↔physical coordinate mapping / JPEG downscale;
   stop emitting the API-rejected `image/rgba` media type
   (`runtime_registry.cppm:1538`).
6. Wire the orphaned `ui/permissions/permission_computer_use.cppm` (compiled,
   never imported; static Renderer, no event handler) into permission dispatch
   with interactivity.
7. Linux host capture/injection is a no-op today (Apple `screencapture`/CGEvent
   only). Irrelevant if a separate computer-use MCP server owns I/O — which is
   the recommended design.

Note: an external computer-use MCP server can already be driven TODAY through
the generic `mcp` tool with screenshots fed back; that is ordinary MCP usage,
not the model's built-in computer loop.

## 2. Teams (herdr-style) — gaps to persistent, watchable, obedient panes

In-process teammates work (`agent_tool.cppm:701-742` spawns a real
`std::thread` running the agent loop; native-store routing + cancel + resume
are real). To reach herdr-style teams:

1. **Capture tmux environment at startup** — call
   `EnvironmentDetection::capture_env(getenv("TMUX"), getenv("TMUX_PANE"))`
   (`swarm_backends.cppm:316`, currently no callers). Without it
   `is_inside_tmux()` is hard-false and every spawn takes the wrong
   external-session path.
2. **Make spawned pane teammates actually run the teammate protocol.** The
   pane launches `cc-repl --team-name …` (`swarm_backends.cppm:981-1001`,
   flags parsed `main.cpp:294-313`) but nothing reads the mailbox
   (`team_helpers.cppm read_inbox` has zero non-test callers), polls tasks, or
   sends idle/completion notifications. Port inbox consumption + TeammateInit/
   Stop hooks into the main loop. **Biggest end-to-end break: panes open but
   the REPL ignores its instructions.**
3. **Implement permission synchronization** — `PermissionSync`
   (`swarm_helpers.cppm:536-556`) is declaration-only (undefined methods);
   TS `permissionSync.ts` is 928 LOC of mailbox request/response + lockfiles.
   Register the `LeaderPermissionBridge` (setters never called).
4. In-process: construct `InProcessTeammateTaskState` on spawn (never created),
   add inbound idle/shutdown handling; define or delete the vestigial
   `InProcessRunner` (`swarm_helpers.cppm:439`).
5. Tmux protocol fidelity: balanced alternating splits (70% first, -v/-h
   targeting), `main-vertical` layout, `list-windows`/recreate swarm-view,
   pane liveness, and leader-exit SIGHUP cleanup killing spawned panes
   (none exists).
6. **Watchability**: add `capture-pane -p` observation (only the dead toy at
   `coordinator/swarm.cppm:358` has it) and feed a live stream into the
   unmounted `ui/teams/*` views from `native_agent_store`/`global_team_store`
   (the team UI is imported by nothing; team_status symbol imported but
   unused; only an integer teammate count reaches the footer).
7. Register model tools under the canonical names **TeamCreate / SendMessage**
   (or add aliases) and ensure **Agent** is registered/dispatched in the
   outer model registry (visible-name listed at runtime_registry:1116 but no
   registration found).
8. Reconnection: define+call `SwarmReconnection::compute_initial_context`.
9. iTerm2 (macOS, lower priority): `It2Setup` methods are declaration-only;
   use `it2 session run` (TS) not `send-text`; probe with `it2 session list`.
10. Delete/quarantine the parallel toy stack `coordinator/swarm.cppm`
    (SwarmManager + second TmuxBackend + unused CoordinatorMode/LoadBalancer/
    HealthChecker) — unreferenced, invites false-confidence greps.

## 3. Local memory — gaps to a working model-driven memory loop

**Capability 1 (basic auto-memory write/read) is non-functional end-to-end.**

1. **Implement the canonical path layer first** (`memdir/paths.cppm`):
   `getAutoMemPath() = $CLAUDE_CONFIG_DIR/projects/<sanitized-canonical-git-root>/memory/`,
   `isAutoMemPath`, `getAutoMemEntrypoint`, enablement chain
   (`CLAUDE_CODE_DISABLE_AUTO_MEMORY`, settings `autoMemoryEnabled`), team =
   `<autoMemPath>/team`. The string `.claude/projects` appears nowhere today;
   five invented stores diverge (`memdir/paths.cppm`, `memdir/memory.cppm:244`,
   `utils/memdir.cppm:166`, `extract_memories.cppm:261`, `remember.cppm:51`).
2. **Inject the memory prompt** — call the existing
   `memdir::build_memory_lines` / `build_combined_memory_prompt`
   (`memdir/memdir.cppm:451,583`) from QueryEngine system-prompt build
   (`query_engine.cppm:838`), mkdir the dir, load/truncate `MEMORY.md`.
   **Without this the model never knows it can save memories** — 0 importers.
   Gaps 1+2 alone restore the model-driven save path immediately.
3. Wire `extract_memories` at turn end: replace the keyword heuristic
   (`extract_memories.cppm:138`, prompt built but never sent) with a real
   forked-agent LLM call (`utils/forked_agent.cppm`, `side_query.cppm` both
   have 0 importers), writing frontmatter `.md` into the canonical path, with
   init + shutdown drain (TS stopHooks.ts:142-153).
4. CLAUDE.md discovery parity in `load_claude_md` (`query_engine.cppm:931`,
   currently nearest-file only): root→cwd walk, `.claude/CLAUDE.md`,
   recursive `.claude/rules/**/*.md`, `CLAUDE.local.md`, user file,
   precedence, @include resolution, then AutoMem MEMORY.md.
5. Team local shared memory: call the real secret guard from
   file_edit/file_write validation; fix `memdir/team_memory.cppm` to use
   `<autoMemPath>/team` (currently invents `.claude/team-memory`).
6. `find_relevant_memories` concrete selector + invocation (lower priority;
   MEMORY.md index injection + explicit Read covers basic recall).
7. Session memory `…/session-memory/summary.md` for compaction (currently
   in-RAM stubs).
8. Delete/consolidate dead duplicates (KV-map remember, ndjson MemoryManager,
   utils/memdir, twin session_memory files, heuristic auto_dream) so future
   wiring picks the TS-compatible implementation.

autoDream: do NOT port now — doubly flag-gated; extractMemories covers the
durable-memory value.

## 4. Bridge — v2 interactive remote-control gaps (phone/web pairing)

v1 `--bridge-daemon` (env register → long-poll work → fork headless child →
wss worker + ingress upload) is real. v2 (drive the interactive REPL remotely)
is sealed scaffolding. To make "pair a device and drive the agent":

1. **Wire the v2 core into a live entrypoint** — export and call
   `init_env_less_bridge_core` / `initReplBridge` (`core.cppm:1215`,
   `init.cppm:534`, currently zero importers) from interactive startup, with
   real OAuth token, the SDK-message adapter (`remote/sdk_message_adapter.cppm`
   is test-only), and inbound-message → QueryEngine routing.
2. Implement POST `/v1/code/sessions/{id}/bridge` for real
   (`session_api.cppm:388` fabricates `worker.<hash>.sig` today); make core
   call the real create/archive functions instead of synthetic overloads.
3. Open the SSE read stream in `ReplV2Transport::connect()` (never opens it)
   and add TLS to `cli/sse_transport.cppm` (raw BSD TCP to https URLs today),
   or route through the existing TLS http SSE client.
4. Port the CCR v2 worker protocol into `cli/ccr_client.cppm`: PUT /worker +
   epoch, heartbeat, batched POST /worker/events (SerialBatchEventUploader),
   delivery acks, 409 epoch-mismatch / expired-JWT, internal-events paging,
   stream coalescing (TS ccrClient.ts:262-998).
5. Make proactive token refresh functional (`TokenRefreshScheduler.on_refresh`
   is log-only).
6. Trusted-device pairing end-to-end: real HTTP enrollment + secure storage
   (`trusted_device.cppm` is a local-file fake), source the token in daemon/main
   (hardcoded nullopt at main.cpp:1399), and the login/OAuth prerequisite
   (absent in C++).
7. Dispatch `repl_bridge_*` store actions from the lifecycle and give the
   `/bridge` command real start/stop actions (the footer wired in commit
   0c6ae13 currently always shows default state).
8. Add TLS to the SSE worker path; remove or complete the misleading skeletons
   (bridge.cppm WS server with no HTTP upgrade + uncalled authenticate(),
   bridge_main abstract spawner, ws://-only bridge/transport.cppm,
   HybridTransport/RemoteIO zero callers).

## Suggested sequencing (value / dependency order)

1. **Computer-use gap 1+2** (emit native tool + screenshot after action) —
   small, high-impact, unblocks the marquee capability on top of working MCP.
2. **Memory gaps 1+2** (canonical path + inject memory prompt) — tiny change,
   immediately makes the model self-improving; then extractMemories (3).
3. **Teams gaps 1+2** (tmux env capture + pane teammate mailbox protocol) —
   turns launched panes from decorative into obedient agents; then permission
   sync (3) and watchability (6).
4. **Bridge v2** is the largest workstream and depends on real cloud endpoints;
   sequence it last (or behind a feature flag), reusing the TLS http client.

## Cross-cutting recommendation

Add a **"production-call-site" gate** to the port process: a module is not
"ported" until a non-test, non-self importer reaches it from
main/QueryEngine. Every major gap above (LSP, prompts, passive feedback,
memory, v2 bridge, team UI, computer loop) is the same root cause — code
written and unit-tested in isolation but never connected to the live path.
A linker/import-reachability CI check (or a grep-based audit in the build)
would catch the entire class.
