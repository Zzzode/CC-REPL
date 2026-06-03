# CC-REPL C++ Architecture Review

> Updated: 2026-06-03
> Scope: Native C++ migration architecture, build entrypoints, command/tool coverage, and validation gates.

## Current Status

The native C++ migration is now the default product build path.

Validated gates:

- `bun run build` configures and builds `cpp_migration` with the `clang-release` CMake preset.
- `dist/cc-repl` is the default runtime entrypoint.
- `dist/cli.js` remains as a compatibility launcher that delegates to the native binary.
- `ctest --test-dir cpp_migration/build/clang-release --output-on-failure` passes.
- `node scripts/cpp-migration-inventory.mjs --strict` passes.
- `bun run migration:e2e` passes native version/help, package start, compatibility launcher, and strict inventory checks.
- `cc-repl --list-runtime-commands` exposes the native slash command registry for runtime inspection.
- `cc-repl --list-runtime-tools` exposes the native tool registry for runtime inspection.
- `cc-repl --simple-ui` can dispatch local slash commands without `ANTHROPIC_API_KEY`.
- `/version` and `--version` now share the same native version constant.
- `/status`, immediate `/version`, diagnostics, and user-agent constants now report the native `1.0.0-cpp` version family.
- `todo_write` parses the TypeScript-compatible `todos` input shape instead of accepting an empty placeholder payload.
- Runtime helper commands no longer return readiness placeholders: local commands now perform real diagnostics, cache clearing, session-history backfill, rate-limit state updates, IDE bridge lockfile inspection, onboarding checks, verifier profile creation, and tool-call JSON validation.

## Architecture Overview

```mermaid
flowchart TD
  package[package.json scripts] --> build[scripts/build.mjs]
  build --> cmake[CMake clang-release preset]
  cmake --> native[cpp_migration/build/clang-release/bin/cc-repl]
  build --> dist[dist/cc-repl]
  build --> compat[dist/cli.js compatibility launcher]
  package --> start[./dist/cc-repl]
  compat --> dist
```

## Native Module Layers

| Layer | CMake target | Responsibility |
| --- | --- | --- |
| Core types and utilities | `cc_types`, `cc_utils` | Shared types, parsing, filesystem, shell, JSON, security helpers |
| Runtime state | `cc_state`, `cc_context`, `cc_session` | App state, context, persistence, session history |
| Product features | `cc_tools`, `cc_commands`, `cc_services` | Tool execution, slash commands, API/MCP/LSP/OAuth integrations |
| Interaction | `cc_ui`, `cc_screens`, `cc_hooks` | Terminal UI, screens, notifications, hook orchestration |
| Integrations | `cc_bridge`, `cc_remote`, `cc_plugins`, `cc_skills_core`, `cc_skills` | IDE bridge, remote sessions, plugin loading, skill primitives, skill execution |
| Entrypoints | `cc_cli`, `cc_entrypoints`, `cc_repl` | CLI parsing, runtime dispatch, native executable |

## Migration Coverage

The strict inventory gate no longer reports command/tool overlap as a completion score. Functional parity is tracked through runtime behavior and targeted migration tests instead.

The strict inventory gate currently reports:
- Runtime command surface: all commands are typed C++ registrations or typed adapters to migrated helper modules; the generic surface fallback shim and readiness placeholder responses have been removed.
- Runtime tool surface: tool dispatch uses registered C++ tool implementations or domain-backed adapters for MCP, LSP, task, plan, worktree, sleep, team, script, and related runtime tools.
- CMake registered source/header entries: complete
- Unregistered C++ compilable files: `0`
- Registered but missing files: `0`
- Blocking migration markers: `0`

Non-blocking placeholder markers remain only where "placeholder" is a legitimate product concept, such as UI input placeholder text, template variable placeholders, and terminal image placeholders.

## Entry Point Policy

The default runtime is native:

```bash
bun run build
bun run start -- --version
```

`start:ts` is retained only as a migration diagnostic path for the TypeScript source tree. It is not the product default.

## Validation Commands

```bash
bun run build
cmake --build --preset clang-release
ctest --test-dir cpp_migration/build/clang-release --output-on-failure
node scripts/cpp-migration-inventory.mjs --strict
bun run migration:e2e
```

## Residual Risks

- Some native modules intentionally consolidate multiple TypeScript files, so file-count parity is not a meaningful completion metric.
- The compatibility launcher is still JavaScript, but it only delegates to the native binary.
- The retained TypeScript diagnostic path does not currently typecheck (`bun run typecheck` fails on missing TS modules, ES lib target drift, React compiler-runtime typings, and union narrowing issues). This is outside the native product gate but remains a cleanup item if `start:ts` must stay usable.
- External workflow commands such as PR autofix and commit-push-PR now perform real local preflight checks, but they still require authenticated GitHub/remote transports for full product-equivalent automation.
- Several integration surfaces still need end-to-end parity validation with real transports: LSP diagnostics, MCP resource/tool calls, IDE bridge message transport, remote execution, plugins, and computer-use capture.
- GoogleTest discovery can occasionally hit CMake's 5 second discovery timeout under heavy relinking; direct `--gtest_list_tests` and reruns complete normally.
