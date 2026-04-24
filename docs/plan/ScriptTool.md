# Script Tool Implementation Plan

## Context

Create a TypeScript Script tool that lets the AI agent write and execute TypeScript code directly as an alternative to Bash. The goal is to provide a safer sandbox, better type safety, and richer capabilities.

## Design Decisions

- **Tool name**: `Script`
- **Default permission**: `fs.read only` (write operations require explicit declaration)
- **Subprocess**: Supported but restricted (requires `subprocess` permission + command allowlist)

**Core advantages:**

- Type safety - TypeScript's strong type system
- Better error handling - explicit try/catch vs implicit shell errors
- Cross-platform consistency - no Windows/macOS/Linux shell differences
- Composability - execute multiple operations in one invocation
- Fine-grained permission control - permission checks per API call

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                     ScriptTool Architecture                         │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   LLM calls ScriptTool                                              │
│   { code: "...", permissions: ["fs.read", "fs.write"] }            │
│              │                                                      │
│              ▼                                                      │
│   ┌─────────────────────────────────────────────────────────────┐  │
│   │ ScriptTool.ts                                                │  │
│   │  - validateInput() syntax checks + dangerous-pattern checks │  │
│   │  - checkPermissions() permission preflight                  │  │
│   │  - call() execution entrypoint                              │  │
│   └─────────────────────────────────────────────────────────────┘  │
│              │                                                      │
│              ▼                                                      │
│   ┌─────────────────────────────────────────────────────────────┐  │
│   │ sandbox.ts - Bun VM isolation                               │  │
│   │  - create VM isolate                                        │  │
│   │  - inject safe APIs                                         │  │
│   │  - timeout/memory limits                                    │  │
│   │  - stdout/stderr capture                                    │  │
│   └─────────────────────────────────────────────────────────────┘  │
│              │                                                      │
│              ▼                                                      │
│   ┌─────────────────────────────────────────────────────────────┐  │
│   │ api.ts - safe API layer                                     │  │
│   │  fs.readFile()     → permission check → FileReadTool logic  │  │
│   │  fs.writeFile()    → permission check → FileWriteTool logic │  │
│   │  fs.glob()         → permission check → GlobTool logic      │  │
│   │  fs.grep()         → permission check → GrepTool logic      │  │
│   │  http.fetch()      → permission check → domain allowlist    │  │
│   └─────────────────────────────────────────────────────────────┘  │
│              │                                                      │
│              ▼                                                      │
│   ┌─────────────────────────────────────────────────────────────┐  │
│   │ permissions.ts - permission controller                      │  │
│   │  - reuse checkReadPermissionForTool()                       │  │
│   │  - reuse checkWritePermissionForTool()                      │  │
│   │  - dynamic permission requests (ask user)                   │  │
│   └─────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## Files to Create

### 1. `src/tools/ScriptTool/constants.ts`

```typescript
export const SCRIPT_TOOL_NAME = 'Script'

export const DEFAULT_TIMEOUT_MS = 30000
export const MAX_TIMEOUT_MS = 120000

export type Permission = 'fs.read' | 'fs.write' | 'network' | 'subprocess'

// Subprocess allowlisted commands
export const ALLOWED_COMMANDS = [
  'git', 'npm', 'yarn', 'pnpm', 'bun',
  'docker', 'docker-compose',
  'cargo', 'rustc',
  'go',
  'python', 'python3', 'pip',
  'node',
]

export const DANGEROUS_PATTERNS = [
  /\beval\s*\(/,
  /new\s+Function\s*\(/,
  /process\./,
  /require\s*\(/,
  /globalThis\./,
]
```

### 2. `src/tools/ScriptTool/types.ts`

- Input/Output type definitions
- Progress types
- Permission types
- API interface types

### 3. `src/tools/ScriptTool/prompt.ts`

- Tool description
- API documentation
- Usage examples

### 4. `src/tools/ScriptTool/permissions.ts`

```typescript
export class ScriptPermissionChecker {
  constructor(
    private context: ToolUseContext,
    private requestedPermissions: Permission[]
  ) {}

  async checkRead(path: string): Promise<PermissionDecision>
  async checkWrite(path: string): Promise<PermissionDecision>
  async checkNetwork(url: string): Promise<PermissionDecision>
}

export function extractPermissionHints(code: string): Permission[]
export function checkForDangerousPatterns(code: string): string[]
```

### 5. `src/tools/ScriptTool/api.ts`

```typescript
export interface SandboxAPIs {
  fs: {
    readFile(path: string, options?): Promise<string>
    writeFile(path: string, content: string): Promise<void>
    appendFile(path: string, content: string): Promise<void>
    exists(path: string): Promise<boolean>
    stat(path: string): Promise<FileStats>
    mkdir(path: string): Promise<void>
    rm(path: string): Promise<void>
    readdir(path: string): Promise<string[]>
    glob(pattern: string): Promise<string[]>
    grep(pattern: string, options?): Promise<GrepResult[]>
  }
  http: {
    fetch(url: string, options?): Promise<Response>
    get(url: string): Promise<Response>
    post(url: string, body: unknown): Promise<Response>
  }
  process: {
    exec(command: string, options?): Promise<ExecResult>  // restricted allowlisted commands
  }
  utils: {
    cwd: string
    env: Record<string, string>
    sleep(ms: number): Promise<void>
  }
  console: {
    log(...args): void
    error(...args): void
  }
}

export function createSandboxAPIs(options: {
  permissions: Permission[]
  checker: ScriptPermissionChecker
  cwd: string
  stdout: (msg: string) => void
  stderr: (msg: string) => void
}): SandboxAPIs
```

### 6. `src/tools/ScriptTool/sandbox.ts`

```typescript
export interface SandboxOptions {
  code: string
  timeoutMs: number
  permissions: Permission[]
  checker: ScriptPermissionChecker
  cwd: string
  abortSignal?: AbortSignal
}

export interface SandboxResult {
  result: unknown
  stdout: string
  stderr: string
  durationMs: number
  error?: Error
}

export async function executeInSandbox(options: SandboxOptions): Promise<SandboxResult>
```

### 7. `src/tools/ScriptTool/ScriptTool.ts`

- `buildTool()` definition
- `inputSchema` / `outputSchema`
- `validateInput()` - syntax checks and dangerous-pattern checks
- `checkPermissions()` - permission preflight
- `call()` - execution entrypoint
- `renderToolUseMessage` / `renderToolResultMessage`

### 8. `src/tools/ScriptTool/UI.tsx`

- React component rendering

## Files to Modify

### `src/tools.ts`

```typescript
// Add import
const ScriptTool = feature('SCRIPT_TOOL')
  ? require('./tools/ScriptTool/ScriptTool.js').ScriptTool
  : null

// Add in getAllBaseTools()
...(ScriptTool ? [ScriptTool] : []),
```

## Implementation Phases

### Phase 1: Core Types & Constants

1. `constants.ts` - tool name and limit constants
2. `types.ts` - type definitions

### Phase 2: Permission System

1. `permissions.ts` - permission checker
2. Reuse existing `checkReadPermissionForTool()` / `checkWritePermissionForTool()`

### Phase 3: Safe APIs

1. `api.ts` - filesystem APIs
2. Network-restricted APIs
3. Utility functions

### Phase 4: Sandbox

1. `sandbox.ts` - Bun VM isolation
2. Timeout-based forced termination
3. Error formatting

### Phase 5: Tool Definition

1. `ScriptTool.ts` - `buildTool()`
2. `prompt.ts` - API docs
3. `UI.tsx` - rendering

### Phase 6: Integration

1. Register tool by modifying `src/tools.ts`
2. Add feature flag

## Security Layers

1. **Static analysis**: detect dangerous patterns (eval, Function, process, require)
2. **Permission declaration**: code must declare required permissions
3. **API gateway**: check permissions on every API call
4. **VM isolation**: no access to Node.js globals
5. **OS sandbox**: optional bubblewrap isolation

## Verification

1. **Unit tests**:
   - Permission-check logic
   - Dangerous-pattern detection
   - API permission gateway

2. **Integration tests**:
   - File read/write operations
   - Network requests (domain restrictions)
   - Timeout-based forced termination

3. **Manual testing**:

   ```bash
   # Test after build
   bun run build
   bun run start

   # Test script execution in Claude Code
   ```

## Example Usage

```typescript
// Read JSON file
{
  code: `
    const content = await fs.readFile('package.json')
    return JSON.parse(content).dependencies
  `,
  permissions: ['fs.read']
}

// Batch file replacement
{
  code: `
    const files = await fs.glob('src/**/*.ts')
    for (const file of files) {
      const content = await fs.readFile(file)
      await fs.writeFile(file, content.replace(/oldName/g, 'newName'))
    }
    return { count: files.length }
  `,
  permissions: ['fs.read', 'fs.write']
}

// Network request
{
  code: `
    const response = await http.get('https://api.example.com/data')
    return await response.json()
  `,
  permissions: ['network']
}
```
