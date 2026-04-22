# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and Development Commands

```bash
# Build the CLI wrapper
bun run build

# Run the CLI (requires build first)
bun run start

# Type check without emitting
bun run typecheck

# Run with specific settings
bun run start:ttadk

# Check version
bun run start:version

# Run with computer use MCP enabled
bun run start:cu
```

## Tech Stack

- **Runtime**: Bun (packageManager: bun@1.3.4)
- **Language**: TypeScript (strict: false in tsconfig)
- **Terminal UI**: React + [Ink](https://github.com/vadimdemedes/ink) (React for CLI)
- **CLI Parsing**: Commander.js (extra-typings)
- **Schema Validation**: Zod v4
- **Code Search**: ripgrep (vendored in src/utils/vendor/ripgrep/)
- **Feature Flags**: Bun's `bun:bundle` feature() for build-time dead code elimination
- **Protocols**: MCP SDK, LSP
- **API**: Anthropic SDK

## Architecture Overview

### Entry Points

- `src/entrypoints/cli.tsx` — Bootstrap entrypoint with fast-path handling for special flags
- `src/main.tsx` — Full CLI initialization, Commander.js setup, Ink renderer

The CLI uses dynamic imports to minimize module evaluation for fast startup paths (e.g., `--version` has zero imports beyond cli.tsx).

### Core Systems

**Tools** (`src/tools/`): Self-contained modules for each tool Claude can invoke. Each defines input schema, permission model, and execution logic. Key tools: BashTool, FileReadTool, FileWriteTool, FileEditTool, GlobTool, GrepTool, WebFetchTool, WebSearchTool, AgentTool, SkillTool, MCPTool, TaskCreateTool, TaskUpdateTool, TeamCreateTool.

**Commands** (`src/commands/`): Slash command implementations (e.g., `/commit`, `/review`, `/compact`, `/mcp`, `/config`, `/doctor`).

**Services** (`src/services/`): External integrations including Anthropic API (`api/`), MCP server management (`mcp/`), OAuth (`oauth/`), LSP (`lsp/`), analytics, plugins, context compression.

**Bridge** (`src/bridge/`): Bidirectional communication for IDE integrations (VS Code, JetBrains). Handles messaging protocol, JWT auth, session management.

**Hooks** (`src/hooks/`): React hooks for UI state, including `toolPermission/` for permission checking on tool invocations.

**State** (`src/state/`): AppState management with store pattern.

### Key Files

- `src/QueryEngine.ts` (~46K lines) — Core LLM API engine: streaming, tool-call loops, thinking mode, retry logic
- `src/Tool.ts` (~29K lines) — Base types and interfaces for all tools
- `src/commands.ts` (~25K lines) — Command registry with conditional imports
- `src/tools.ts` — Tool registry

### Feature Flags

Dead code elimination via Bun's `bun:bundle`:

```typescript
import { feature } from 'bun:bundle'

// Inactive code is stripped at build time
if (feature('SOME_FLAG')) {
  // This block is eliminated if flag is false
}
```

Notable flags: `PROACTIVE`, `KAIROS`, `BRIDGE_MODE`, `DAEMON`, `VOICE_MODE`, `AGENT_TRIGGERS`, `MONITOR_TOOL`, `TEMPLATES`, `BG_SESSIONS`, `BYOC_ENVIRONMENT_RUNNER`, `SELF_HOSTED_RUNNER`.

### Startup Optimization

- Parallel prefetch: MDM settings, keychain reads, API preconnect fired as side-effects before heavy module evaluation
- Lazy loading: Heavy modules (OpenTelemetry ~400KB, gRPC ~700KB) deferred via dynamic `import()`

### Agent Swarms

Sub-agents spawned via `AgentTool` with `coordinator/` handling multi-agent orchestration. `TeamCreateTool` enables team-level parallel work with shared task lists.

### Skill System

Reusable workflows in `src/skills/` executed through `SkillTool`. Users can add custom skills.

## Important Notes

- The `stubs/` directory contains placeholder packages for internal Anthropic dependencies (`@ant/*`)
- Build output is a thin wrapper in `dist/cli.js` that imports `src/entrypoints/cli.tsx` directly
- Source maps reference the original leaked source from Anthropic's R2 storage
