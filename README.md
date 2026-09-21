# Loom

A C++23 terminal agent harness.

Loom is a CLI that runs an LLM agent loop against a model endpoint of your
choosing: streaming responses, tool calls, permissions, sub-agents, and an
interactive FTXUI interface.

## Status

Under active development. It builds and its test suite passes on Linux
(Homebrew LLVM) and macOS (CI); it has no releases and no stability promise.

## Building

Requires CMake ≥ 3.28, Ninja, and a Clang that ships `clang-scan-deps` (C++23
named modules need a matched `clang++` / `clang-scan-deps` pair; AppleClang
does not qualify).

```bash
cmake --preset release
cmake --build --preset release -j8
ctest --preset release -j1
```

Configuring without a preset is a deliberate error — it would otherwise pick a
toolchain that cannot build this tree.

### On a Linux dev box

The committed presets target the macOS CI runner. On Linux, copy the local
preset (gitignored, since it holds absolute paths) and adjust the compiler
prefixes to your own LLVM install:

```bash
cp CMakeUserPresets.json.example CMakeUserPresets.json   # then replace the <PLACEHOLDER> paths
cmake --preset local-linux
cmake --build --preset local-linux -j8
ctest --preset local-linux -j1
```

Dependencies are fetched at configure time. On a machine with no network
access, place the six pinned archives under `.deps-cache/<name>-src/` and CMake
will use them instead — it probes for that directory automatically.

## Running

```bash
./build/release/bin/loom                    # interactive REPL
./build/release/bin/loom --headless "hi"    # one-shot, no UI
./build/release/bin/loom --help             # all flags
```

Point it at an endpoint with the usual environment:

```bash
ANTHROPIC_API_KEY=... ANTHROPIC_BASE_URL=... ./build/release/bin/loom
```

An OpenAI-compatible endpoint works too — set `LOOM_WIRE_API=openai` (or the
`wire_api` config key) and point `ANTHROPIC_BASE_URL` at it. Loom speaks both
wire formats through a seam that `src/query/wire_protocol.cppm` defines; the
engine itself is format-agnostic.

## Configuration

    config dir   $LOOM_CONFIG_DIR > ~/.loom > ~/.agents > ~/.claude   (read)
                 $LOOM_CONFIG_DIR > ~/.loom                          (write)
    memory file  LOOM.md > AGENTS.md > CLAUDE.md   (per directory, walking up)

Read follows the cascade; write does not — Loom creates its own state under
`~/.loom` rather than writing into another tool's directory. Session transcripts
land in `~/.loom/sessions/`, API dumps in `~/.loom/dump-prompts/`.

## Layout

```
src/query/       the agent engine and the wire-backend seam
src/tools/       tool implementations (bash, file ops, MCP, agents, …)
src/commands/    slash commands
src/ui/          FTXUI interface
src/services/    MCP, LSP, API clients, plugins
benchmarks/pare/ benchmark case data (read by the pare-benchmark binary)
```

The codebase is C++23 named modules. A module's name need not match its file
path, so moving a file usually needs only a `CMakeLists.txt` update.

For contributors — build details, conventions, and the cross-module coupling
hazards this code is unusually prone to — see `CLAUDE.md`. Design intent
inherited from the original TypeScript implementation is recorded in
`docs/decisions/design-decisions.md`.

## License

Not yet chosen.
