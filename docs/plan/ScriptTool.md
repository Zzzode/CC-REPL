# Script Tool 实现计划

## Context

创建一个 TypeScript Script 工具，让 AI agent 能够直接写 TypeScript 代码并执行，作为 Bash 的替代方案。目标是提供更安全的沙箱环境、更好的类型安全和更丰富的功能。

## 设计决策

- **工具名**: `Script`
- **默认权限**: `fs.read only`（写操作需要显式声明）
- **Subprocess**: 支持但受限（需要 `subprocess` 权限 + 命令白名单）

**核心优势**：

- 类型安全 - TypeScript 强类型系统
- 更好的错误处理 - try/catch vs shell 隐式错误
- 跨平台一致性 - 无 Windows/macOS/Linux shell 差异
- 可组合性 - 单次调用执行多操作
- 细粒度权限控制 - 按 API 调用检查权限

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        ScriptTool 架构                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   LLM 调用 ScriptTool                                               │
│   { code: "...", permissions: ["fs.read", "fs.write"] }            │
│              │                                                      │
│              ▼                                                      │
│   ┌─────────────────────────────────────────────────────────────┐  │
│   │ ScriptTool.ts                                                │  │
│   │  - validateInput() 语法检查、危险模式检测                    │  │
│   │  - checkPermissions() 权限预检                               │  │
│   │  - call() 执行入口                                           │  │
│   └─────────────────────────────────────────────────────────────┘  │
│              │                                                      │
│              ▼                                                      │
│   ┌─────────────────────────────────────────────────────────────┐  │
│   │ sandbox.ts - Bun VM 隔离                                     │  │
│   │  - 创建 VM isolate                                           │  │
│   │  - 注入安全 API                                              │  │
│   │  - 超时/内存限制                                             │  │
│   │  - stdout/stderr 捕获                                        │  │
│   └─────────────────────────────────────────────────────────────┘  │
│              │                                                      │
│              ▼                                                      │
│   ┌─────────────────────────────────────────────────────────────┐  │
│   │ api.ts - 安全 API 层                                         │  │
│   │  fs.readFile()     → 权限检查 → FileReadTool 逻辑           │  │
│   │  fs.writeFile()    → 权限检查 → FileWriteTool 逻辑          │  │
│   │  fs.glob()         → 权限检查 → GlobTool 逻辑               │  │
│   │  fs.grep()         → 权限检查 → GrepTool 逻辑               │  │
│   │  http.fetch()      → 权限检查 → 域名白名单                  │  │
│   └─────────────────────────────────────────────────────────────┘  │
│              │                                                      │
│              ▼                                                      │
│   ┌─────────────────────────────────────────────────────────────┐  │
│   │ permissions.ts - 权限控制器                                  │  │
│   │  - 复用 checkReadPermissionForTool()                        │  │
│   │  - 复用 checkWritePermissionForTool()                       │  │
│   │  - 动态权限请求 (ask user)                                   │  │
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

// Subprocess 白名单命令
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

- Input/Output 类型定义
- Progress 类型
- Permission 类型
- API 类型接口

### 3. `src/tools/ScriptTool/prompt.ts`

- 工具描述
- API 文档
- 使用示例

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
    exec(command: string, options?): Promise<ExecResult>  // 受限白名单命令
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

- buildTool() 定义
- inputSchema/outputSchema
- validateInput() - 语法检查、危险模式
- checkPermissions() - 权限预检
- call() - 执行入口
- renderToolUseMessage/renderToolResultMessage

### 8. `src/tools/ScriptTool/UI.tsx`

- React 组件渲染

## Files to Modify

### `src/tools.ts`

```typescript
// 添加导入
const ScriptTool = feature('SCRIPT_TOOL')
  ? require('./tools/ScriptTool/ScriptTool.js').ScriptTool
  : null

// 在 getAllBaseTools() 中添加
...(ScriptTool ? [ScriptTool] : []),
```

## Implementation Phases

### Phase 1: Core Types & Constants

1. `constants.ts` - 工具名、限制常量
2. `types.ts` - 类型定义

### Phase 2: Permission System

1. `permissions.ts` - 权限检查器
2. 复用现有 `checkReadPermissionForTool()` / `checkWritePermissionForTool()`

### Phase 3: Safe APIs

1. `api.ts` - 文件系统 API
2. 网络受限 API
3. 工具函数

### Phase 4: Sandbox

1. `sandbox.ts` - Bun VM 隔离
2. 超时强制终止
3. 错误格式化

### Phase 5: Tool Definition

1. `ScriptTool.ts` - buildTool()
2. `prompt.ts` - API 文档
3. `UI.tsx` - 渲染组件

### Phase 6: Integration

1. 修改 `src/tools.ts` 注册工具
2. 添加 feature flag

## Security Layers

1. **静态分析**: 检测危险模式 (eval, Function, process, require)
2. **权限声明**: 代码必须声明所需权限
3. **API 网关**: 每个 API 调用检查权限
4. **VM 隔离**: 无 Node.js 全局变量访问
5. **OS 沙箱**: 可选的 bubblewrap 隔离

## Verification

1. **单元测试**:
   - 权限检查逻辑
   - 危险模式检测
   - API 权限网关

2. **集成测试**:
   - 文件读写操作
   - 网络请求（域名限制）
   - 超时强制终止

3. **手动测试**:

   ```bash
   # 构建后测试
   bun run build
   bun run start

   # 在 Claude Code 中测试脚本执行
   ```

## Example Usage

```typescript
// 读取 JSON 文件
{
  code: `
    const content = await fs.readFile('package.json')
    return JSON.parse(content).dependencies
  `,
  permissions: ['fs.read']
}

// 批量文件替换
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

// 网络请求
{
  code: `
    const response = await http.get('https://api.example.com/data')
    return await response.json()
  `,
  permissions: ['network']
}
```
