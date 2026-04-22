import type { Tool } from '../../Tool.js'
import { AgentTool } from '../AgentTool/AgentTool.js'
import { FileEditTool } from '../FileEditTool/FileEditTool.js'
import { FileReadTool } from '../FileReadTool/FileReadTool.js'
import { FileWriteTool } from '../FileWriteTool/FileWriteTool.js'
import { NotebookEditTool } from '../NotebookEditTool/NotebookEditTool.js'
import { WebFetchTool } from '../WebFetchTool/WebFetchTool.js'

let _primitiveTools: readonly Tool[] | undefined

/**
 * ScriptTool 沙箱上下文内可直接调用的"原始工具"集合。
 *
 * 仅包含纯 TypeScript 实现、不 spawn 任何子进程 / 外部二进制的工具：
 *   - FileReadTool     → fs/promises
 *   - FileWriteTool    → fs/promises
 *   - FileEditTool     → fs/promises
 *   - NotebookEditTool → fs/promises
 *   - AgentTool        → 进程内事件循环（Local/Remote Agent Task）
 *   - WebFetchTool     → globalThis.fetch（纯 TS，无子进程；域名白名单/权限已内置）
 *
 * 显式禁止 Bash / Glob / Grep：三者底层都通过 ChildProcess 调用外部程序
 * （bash shell、ripgrep），会突破"纯 JS 执行"的语义边界。
 *
 * 懒加载 getter —— 与 REPLTool/primitiveTools.ts 同款策略：
 * 工具注册表存在循环依赖链（ScriptTool → primitiveTools → FileReadTool →
 * tools registry → ScriptTool），顶层 const 会命中 TDZ 报错
 * "Cannot access before initialization"，延迟到调用时构造数组即可规避。
 */
export function getScriptPrimitiveTools(): readonly Tool[] {
  return (_primitiveTools ??= [
    FileReadTool,
    FileWriteTool,
    FileEditTool,
    NotebookEditTool,
    AgentTool,
    WebFetchTool,
  ])
}
