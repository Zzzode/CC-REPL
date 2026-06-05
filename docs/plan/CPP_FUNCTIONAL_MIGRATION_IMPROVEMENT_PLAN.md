# CC-REPL C++ Functional Migration Improvement Plan

> Updated: 2026-06-05
> Scope: functional migration parity only. This plan intentionally excludes lint, TypeScript typecheck cleanup, launcher cleanup, entrypoint polish, and unrelated engineering aftercare.

## Current Verdict

The C++ migration is not functionally complete yet.

The native runtime is strong enough to be treated as the default smoke-validated runtime surface: it builds, registers all current C++ sources, exposes the expected command/tool surface, and passes unit and migration smoke tests. However, several product-level behaviors are still only partially migrated, only tested through local fixtures, or implemented as in-memory/local adapters where the TypeScript runtime expects real server, bridge, swarm, or external transport behavior.

## Evidence Snapshot

Commands verified on 2026-06-05:

```bash
node scripts/cpp-migration-inventory.mjs --strict
bun run build
ctest --test-dir cpp_migration/build/clang-release --output-on-failure
bun run migration:e2e
./dist/cc-repl --list-runtime-commands | wc -l
./dist/cc-repl --list-runtime-tools | wc -l
```

Observed results:

- TypeScript source files: 1,946.
- C++ source files: 1,095, including 1,091 `.cppm` and 4 `.cpp` files.
- CMake registered source/header entries: 1,095.
- Unregistered C++ compilable files: 0.
- Registered but missing files: 0.
- Blocking migration markers: 0.
- Non-blocking placeholder markers: 78.
- Native runtime commands: 100.
- Native runtime tools: 47.
- `bun run build`: passed.
- CTest: 345/345 passed.
- `bun run migration:e2e`: passed.

This proves registration, buildability, and broad smoke coverage. It does not prove product-equivalent parity for behavior-heavy paths.

## Functional Parity Bar

A feature is functionally migrated only when all of these are true:

- The native runtime exposes the user-visible behavior through the product path or the runtime dispatch path that will replace the TypeScript path.
- The implementation uses the real domain behavior, not a display-only response, fake completion, or in-memory-only adapter unless TypeScript has the same constraint.
- Protocol-dependent features are validated against a real or protocol-faithful local fixture, including auth, reconnect, cancellation, and error paths where those are part of the feature.
- The validation gate includes a focused test or E2E check that would fail if the native path silently fell back to a placeholder.

## Completed Functional Surface

The following areas should be treated as migrated for the current parity bar, subject to continued regression coverage:

- Native binary build and compatibility launcher generation.
- Strict C++ migration inventory with no CMake registration gaps and no blocking migration markers.
- Runtime command and tool registration: 100 commands and 47 tools are exposed by the native runtime.
- Simple UI slash command dispatch without requiring an API key for slash-only commands.
- Core command/tool smoke behavior covered by CTest and `migration:e2e`.
- Core file, shell, grep/glob, edit/write/read, task, todo, sleep, worktree, web fetch/search, LSP, MCP, and command registry surfaces at the registered native-dispatch level.
- AgentTool single-agent execution has moved beyond name overlap: it resolves agent definitions, applies model/system prompt/max-turn/tool constraints, supports preloaded skills, resolves MCP context, executes a streaming API loop, runs tool-use rounds through the native registry, records transcript state, and supports background execution when a registry is attached.
- MCP core is no longer stdio-only: stdio, SSE, and streamable HTTP transports are implemented; headers helpers, stored bearer tokens, 401-to-auth-needed status, tool/resource/prompt list refresh, prompt get, roots handling, and elicitation handler pieces have focused tests.
- Server route handlers exist for `/message`, `GET /sessions`, and `/compact`, and the route handler behavior is covered by tests.
- IDE MCP lockfile and WebSocket MCP tool calls have local fixture coverage.

## Remaining Work

### P0. Wire the Native Server and Direct-Connect Product Path

Current state:

- `cpp_migration/src/server/server_routes.cppm` defines route handlers for `/message`, `GET /sessions`, and `/compact`.
- `cpp_migration/src/server/server_main.cppm` only mounts `GET /health` and `GET /status` on the concrete HTTP server.
- `cpp_migration/src/main.cpp` does not expose a native server/headless/direct-connect runtime path.
- The TypeScript direct-connect client expects `POST /sessions` to return `session_id`, `ws_url`, and optional `work_dir`, then uses a WebSocket stream-json/control protocol.

Required work:

- Mount the real route table from `server_routes.cppm` into `HttpServer` instead of maintaining a separate health/status-only server.
- Add `POST /sessions` session creation with the TypeScript-compatible response shape.
- Add a native WebSocket session loop that accepts stream-json user messages, returns SDK-compatible output messages, and supports keep-alive, disconnect, and error framing.
- Implement the permission control request/response loop used by direct-connect clients.
- Support interrupt/cancel requests for the active native query.
- Persist session state consistently across `/message`, `/sessions`, `/compact`, and WebSocket sessions.

Acceptance gate:

- Add an E2E fixture that starts the native server, creates a session through `POST /sessions`, connects to `ws_url`, sends a stream-json user message, receives SDK-compatible assistant/system output, answers a permission request, sends an interrupt, lists sessions, and compacts the same session.
- The test must hit the concrete HTTP/WebSocket server, not just `get_default_routes()`.

### P0. Replace Bridge Local Adapters with Product-Equivalent Remote Control

Current state:

- `cpp_migration/src/bridge/transport.cppm` marks WebSocket and HTTP polling transports connected without opening real network connections; sent payloads are stored in local vectors.
- `cpp_migration/src/bridge/api.cppm` posts registration requests, but returns synthetic environment IDs and secrets instead of parsing the real response.
- `poll_for_work()` performs the GET call but always returns no work after a successful response.
- Bridge tests cover local transport state, buffering, and safe ID validation, not a full remote work lifecycle.

Required work:

- Implement real Bridge WebSocket and HTTP polling transports with auth token handling, inbound message parsing, outbound delivery, reconnect/backoff, and close/error semantics.
- Parse environment registration responses from the CCR bridge API instead of manufacturing IDs.
- Parse `poll_for_work()` payloads into work items and route them into the native session runner.
- Implement acknowledge, stop, completion, progress, and permission/control response flows against a protocol-faithful local CCR fixture.
- Validate JWT/work-secret/trusted-device behavior through the product bridge path.

Acceptance gate:

- Add a bridge lifecycle E2E fixture: register environment, poll work, acknowledge work, stream session output, handle permission control, publish progress/completion, stop work, and recover from reconnect.
- The test must fail if Bridge transport falls back to local vector storage only.

### P1. Complete Agent Swarm, Team, and Durable Background Lifecycle

Current state:

- `AgentTool` has real streaming sub-agent execution and background thread execution when a registry is attached.
- `send_message` uses an in-process singleton queue only.
- `team_create` and `team_delete` runtime tools currently operate on an in-memory team store.
- `cpp_migration/src/tools/spawn_multi_agent.cppm` returns synthetic local completion text instead of running real AgentTool loops.
- The richer `team_create.cppm` and `team_delete.cppm` file/worktree cleanup APIs are declared but not wired into the runtime tool path.

Required work:

- Route team creation through a single production implementation that persists team files, shared task lists, member metadata, and cleanup state.
- Spawn team members through the same AgentTool execution path used by standalone agents.
- Make `send_message` durable enough for background agents, resumed sessions, and cross-thread or cross-process delivery as required by the TypeScript behavior.
- Implement team deletion cleanup for team directories, transcripts, background tasks, and worktrees.
- Implement worktree and remote isolation semantics for agents and teams instead of only recording requested isolation values.
- Finish fork-subagent context inheritance, cancellation, progress, transcript, result schema, and resume behavior.

Acceptance gate:

- Add tests where `team_create` spawns at least two native agents through a streaming API fixture, assigns shared tasks, delivers `send_message` continuation, records transcripts, supports cancellation, resumes state after restart, and cleans all team/worktree artifacts through `team_delete`.

### P1. Close Remaining MCP Product Parity

Current state:

- Core MCP stdio, SSE, and streamable HTTP transports are implemented and tested with local fixtures.
- Remote config parsing, headers helper, stored bearer token injection, 401 auth classification, plugin-provided MCP discovery, tool/resource/prompt list refresh, prompt get, roots handling, and elicitation handler pieces are present.
- XAA still returns unavailable because the external IdP flow is not implemented.
- OAuth exists, but product-equivalent browser-open/callback/token-refresh behavior still needs real end-to-end coverage.

Required work:

- Implement the XAA external IdP flow and token exchange.
- Add real browser-open or explicit authorization URL handling for OAuth, including callback timeout, cancellation, token refresh, and token revocation paths.
- Expose MCP prompt invocation through the user-facing command/tool surface if it is part of the TypeScript behavior.
- Finish richer MCP tool schema registration, output truncation/classification parity, and UI-visible reconnect/backoff status.
- Wire elicitation policy/responder behavior into the actual runtime path that handles MCP server requests.

Acceptance gate:

- Add a remote MCP E2E matrix covering stdio, SSE, streamable HTTP, OAuth success, OAuth cancellation, XAA success, 401-to-auth retry, reconnect/list_changed refresh, prompt invocation, elicitation approval/deny, and output truncation.

### P1. Match Session, Compaction, and Context Semantics

Current state:

- `QueryEngine::compact_conversation()` and `/compact` have deterministic local compaction behavior and tests.
- `server_routes.cppm` compacts persisted session message lines with a deterministic summary boundary.
- `session_memory_compact.cppm` currently tracks an in-memory token count and clamps it to the requested target.
- The old parity checklist still tracks session persistence and auto-compact parity as open.

Required work:

- Define the TypeScript-equivalent semantics for manual compact, auto-compact, compact boundary metadata, and session memory compaction.
- Replace in-memory session memory counters with durable session-scoped state where TypeScript persists or resumes that state.
- Ensure auto-compact triggers at the same token thresholds and preserves the same message categories.
- Decide whether C++ must use model-generated summarization or deterministic summarization for parity; implement the chosen parity target consistently.
- Validate resume/list/archive behavior across native UI, server routes, and direct-connect sessions.

Acceptance gate:

- Add tests for manual compact, auto-compact threshold triggering, resume after compaction, persisted compact boundary metadata, session memory compaction, and direct-connect compact behavior over the same session.

### P1. Finish Permission, Streaming UI, Thinking, Cost, and Keyboard Parity

Current state:

- Native components parse thinking blocks and render several UI states in tests.
- The permission hook, cost hook, keyboard handling, and streaming rendering have partial native coverage.
- The remaining checklist still marks streaming rendering, permission system, keyboard shortcuts, thinking display, cost tracking, and auto-compact parity as open.

Required work:

- Match TypeScript permission behavior for local tools, MCP tools, background agents, swarm workers, bridge sessions, and direct-connect sessions.
- Validate streaming response rendering for text, thinking, tool-use, tool-result, error, cancellation, and retry states.
- Persist and display cost tracking with the same session/monthly budget semantics as TypeScript.
- Complete keyboard shortcut parity for the interactive runtime, including multiline editing, history, vim mode, cancellation, and permission prompts.
- Validate thinking display collapse/expand and signature handling through the actual UI path.

Acceptance gate:

- Add UI/runtime tests that exercise permission prompts, denial persistence, streaming tool-use rendering, thinking block display, budget warnings, keyboard shortcuts, and cancellation from the native runtime.

### P2. Validate Platform and External Integration Parity

Current state:

- PowerShell provider behavior is tested at the provider level, but the runtime `powershell` tool only executes on Windows.
- IDE MCP WebSocket fixtures exist, but full IDE bridge message transport remains a residual risk.
- Remote execution, plugin workflows, and computer-use capture still need real transport validation.

Required work:

- Run and harden the PowerShell runtime tool on Windows.
- Add real or protocol-faithful E2E coverage for IDE diagnostics, remote execution, plugin-provided tools/commands/MCP servers, and computer-use capture.
- Validate external workflow commands that depend on GitHub, remote runners, or authenticated cloud transports with mocked and authenticated-real modes where feasible.

Acceptance gate:

- Add a platform/external integration validation matrix and require each feature to have either a protocol fixture or a documented real-environment validation script.

## Work Order

1. Close P0 server/direct-connect first because it is the largest user-visible product gap and several later features depend on session/control protocol correctness.
2. Close P0 bridge remote control next because it has the highest risk of false confidence from local adapter tests.
3. Close Agent swarm/team lifecycle after server and bridge protocols are stable, so background delivery, cancellation, and transcript persistence can reuse the same session/control foundations.
4. Close MCP parity with a remote E2E matrix rather than more unit-only coverage.
5. Close compaction/session semantics and UI/runtime parity as focused vertical slices with product-path tests.
6. Run the platform/external validation matrix before declaring functional migration complete.

## Definition of Done

Functional migration can be declared complete only when:

- All P0 and P1 items above have product-path tests or protocol-faithful E2E fixtures.
- The validation bundle passes:

```bash
bun run build
ctest --test-dir cpp_migration/build/clang-release --output-on-failure
node scripts/cpp-migration-inventory.mjs --strict
bun run migration:e2e
```

- The direct-connect, bridge, MCP remote, swarm/team, compact/session, permission/UI, and platform/external validation matrices all pass.
- No remaining functional item relies on a fake completion string, in-memory-only adapter, or display-only command response where the TypeScript runtime provided real behavior.
- Remaining work, if any, is only lint, TypeScript diagnostic cleanup, launcher/entrypoint polish, documentation freshness, or other non-functional engineering aftercare.
