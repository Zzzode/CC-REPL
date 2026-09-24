# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

**Loom** — a C++23 CLI agent harness. A single self-contained project: no
TypeScript, no Bun, no npm. The tree was ported from a TypeScript codebase that
has since been deleted; `docs/decisions/design-decisions.md` records the design
intent that used to live there.

## Build and test

```bash
cmake --preset debug      # configure (also: release, asan)
cmake --build --preset debug -j8
ctest --preset debug -j1

# or, the whole loop at once:
cmake --workflow --preset dev     # configure + build debug
cmake --workflow --preset ci      # configure + build + test release
cmake --workflow --preset check   # configure + build + test asan
```

Configuring without a preset (or an explicit `CMAKE_TOOLCHAIN_FILE`) is a
`FATAL_ERROR` by design: C++23 named modules need a matched `clang++` /
`clang-scan-deps` pair, and a silent fallback would be hard to diagnose.
AppleClang is rejected outright — it ships no `clang-scan-deps`.

**Run tests serially (`-j1`).** There are pre-existing timing-sensitive flakes
under `ctest -j$(nproc)`; `-j1` is deterministic and is the signal that counts.

### Building on this Linux dev box

The committed presets target the macOS CI runner and will not configure here.
Use the machine-local preset (gitignored, because it holds absolute paths):

```bash
cmake --preset local-linux            # debug
cmake --preset local-linux-release    # release
cmake --build --preset local-linux -j8
ctest --preset local-linux -j1
```

It pins Homebrew LLVM 22, points at the offline dependency cache, uses system
(not brew) OpenSSL/curl headers, and links against brew's glibc 2.38. Release
forces `-O0 -DNDEBUG` with LTO off, because the Homebrew LLVM 22 optimizer
crashes on this tree.

**If configuration fails with a FetchContent download error**, `.deps-cache/`
is missing or incomplete. It holds the six pinned dependency archives; this box
has no github.com access, so they cannot be re-fetched. `CMakeLists.txt` probes
for `.deps-cache/<dep>-src/` automatically — no flags needed when it is present.

## Architecture

**Modules, not headers.** The tree is C++23 named modules (`export module
cc.<area>.<thing>;`), built with `-fmodules-reduced-bmi`. Two consequences
worth internalizing:

- **A module's name does not have to match its path.** `export module
  cc.ui.design.tokens;` can live in `ui/design/tokens.cppm`. Moving a file
  therefore usually needs only a `CMakeLists.txt` path update — not an import
  rewrite. This is what makes directory restructuring cheap here.
- **Importers are found by module name, not filename.** To check whether a
  module is used, grep for `import cc.area.thing;`, and cover `tests/` too.

**Build layout.** `src/CMakeLists.txt` holds global/project setup and, in
dependency order, one `include()` per target pulling a file from
`src/cmake/targets/`. `include()` (not `add_subdirectory()`) is deliberate: it
keeps every target in one CMake scope, so the variables and the tree-sitter
conditional at the top are visible throughout — `add_subdirectory()` would add
a directory scope and change evaluation order. When adding a target, add its
file under `src/cmake/targets/` and an `include()` in the same dependency order.

**`cc_ui` is intentionally one target — do not split it to "speed up the
build".** The module graph is a DAG (verified: 217 ui modules, zero module-level
cycles), but grouped by the responsibility directories (`foundation`, `dialogs`,
`messages`, …) nine of them collapse into one strongly-connected component
(e.g. `foundation ↔ chrome`, `dialogs ↔ widgets`, `messages ↔ prompt`), so they
cannot become separate static libraries without a library-level cycle. More
importantly it would not help anyway: with named modules the recompile fan-out
is driven by the BMI/import graph, not the library boundary — touching
`design_tokens` already forces ~62 module recompiles regardless of how the
archives are split. The single FILE_SET also lets clang-scan-deps resolve the
intra-`cc.ui.*` imports both ways.

### Layout

| Path | What |
|---|---|
| `src/query/` | **The engine.** `query_engine.cppm` owns the streaming loop, tool-call loop, thinking mode, retry. Start here for anything about model interaction. |
| `src/query/wire_*.cppm` | The wire-protocol seam. `wire_protocol.cppm` defines `WireBackend`; `wire_anthropic.cppm` and `wire_openai.cppm` implement it. The engine builds a vendor-neutral `RequestInput` and never serializes a wire format itself. |
| `src/tools/` | Tool implementations, each with its input schema, permission model, and execution. |
| `src/commands/` | Slash commands. Registered via `command_registry_init_*.cpp`. |
| `src/ui/` | FTXUI interface. **Not Ink, not React** — do not port React idioms into it. Cut by responsibility: `foundation/` (tokens, theme, figures, primitives), `chrome/` (layout, renderer, terminal I/O), `widgets/` (reusable controls), `visual/` (markdown/diff rendering), `messages/`, `dialogs/`, `permissions/`, `prompt/`, `screens/`, `features/{agents,teams,tasks,plugins,mcp}/`, `tools/` (tool-UI registry), and `app/` (the top-level app orchestrator shards). One `cc_ui` target — see the build-layout note above. |
| `src/services/` | External integrations: MCP, LSP, API clients, plugins. |
| `src/state/` | AppState store and reducers. |
| `src/constants/paths.cppm` | **The single source for config/memory path resolution.** Both cascades live here; delegate to it rather than hardcoding paths. |
| `benchmarks/pare/` | Benchmark case data. The `pare-benchmark` binary reads the JSON by CWD-relative path. |

### Configuration and data paths

    config dir   $LOOM_CONFIG_DIR > ~/.loom > ~/.agents > ~/.claude   (read)
                 $LOOM_CONFIG_DIR > ~/.loom                          (write)
    memory file  LOOM.md > AGENTS.md > CLAUDE.md   (per directory, walking up)

Read follows the cascade; **write never does.** Writing into another tool's
config directory would interleave two tools' state. Both cascades are
implemented in `src/constants/paths.cppm` — eight call sites previously
reimplemented the lookup and silently drifted.

### Wire backends

`wire_api` config key or `LOOM_WIRE_API` env selects the backend; unset ⇒
Anthropic. Credentials reach the wire through the single decision point in
`query/wire_anthropic.cppm` (Bearer when a token is set, else `x-api-key`).
`ANTHROPIC_API_KEY` / `ANTHROPIC_AUTH_TOKEN` are read as user-supplied
credentials for the user's own endpoint — there is no account system and no
login.

## Debug traces

Every session persists two traces, both under the storage dir
(`~/.loom/sessions/`):

- `~/.loom/sessions/<session_id>/messages.jsonl` — the full conversation: every
  message appended to the engine, with tool-use/tool-result/text/thinking
  blocks. One JSON object per line.
- `~/.loom/dump-prompts/<session_id>.jsonl` — the API request body (messages
  array, system prompt, tool schemas) and the parsed response.

Wired in `src/ui/app_constructor.cpp` via `set_session_storage` /
`set_dump_prompts_dir`. Use them to diagnose duplicate tool-call rows, missing
tool output, and wrong tool status. See `.agents/skills/debug-session/SKILL.md`.

## Conventions

- **All code comments and docs in English.**
- **Reviews are agent-run, never user-run.** Every review in this project —
  code review, design review, RFC stage gates, production-readiness review,
  approve/request-changes — is performed by Claude agents, not the user. Do
  not ask the user to read a diff, judge a design, or "sign off"; the user is
  informed of outcomes, never assigned review work. For a consequential gate,
  spawn one or more independent (adversarial where it matters) review agents,
  address or explicitly rebut their findings, and record the agent identity
  as the reviewer. Proceed through gates on the agents' verdict.
- **`-Werror` is on** (`-Wall -Wextra -Wpedantic`). New warnings fail the build.
- **No hardcoded RGB** — use palette/design tokens (`src/ui/design/`).
- **No constant-frequency render ticker** — the FTXUI UI is event-driven.
- **FTXUI components must be held by state**, not reconstructed per render.
- **Every change builds debug *and* release, with ctest green**, before committing.
- Prefer deleting dead code to fixing it.

### A hazard specific to this codebase

Cross-module couplings are often **string- or shape-based**, so a change on one
side silently breaks the other rather than failing to compile. Examples:

- `tools/runtime_registry.cppm`'s `parse_lsp_action` mirrors
  `lsp_action_name()` in `lsp_tool.cppm`; a mismatch silently falls through to
  `LspAction::Symbols` — a *wrong answer*, not an error.
- The `<task_notification>` / `<status>` / `<summary>` tag format is produced by
  three modules (`local_agent_task`, `local_shell_task`, `runtime_registry`) and
  consumed by `ui/messages/collapse_background_bash.cppm`; changing the
  emitters' spelling silently stops the collapsing.

`docs/decisions/design-decisions.md` catalogues these — consult it before
changing a wire shape, a registry key, or a tag format.

## Historical documents

`docs/` holds audit reports and plans written while the TypeScript reference
tree still existed. They contain paths that no longer resolve, and are kept
unedited as records of what was found. `docs/README.md` says which are current.
