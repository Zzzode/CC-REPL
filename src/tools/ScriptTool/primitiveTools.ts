import type { Tool } from '../../Tool.js'
import { AgentTool } from '../AgentTool/AgentTool.js'
import { FileEditTool } from '../FileEditTool/FileEditTool.js'
import { FileReadTool } from '../FileReadTool/FileReadTool.js'
import { FileWriteTool } from '../FileWriteTool/FileWriteTool.js'
import { NotebookEditTool } from '../NotebookEditTool/NotebookEditTool.js'
import { WebFetchTool } from '../WebFetchTool/WebFetchTool.js'

let _primitiveTools: readonly Tool[] | undefined

/**
 * Set of "primitive tools" directly callable inside the ScriptTool sandbox context.
 *
 * Includes only pure TypeScript implementations that do not spawn subprocesses or external binaries:
 *   - FileReadTool     → fs/promises
 *   - FileWriteTool    → fs/promises
 *   - FileEditTool     → fs/promises
 *   - NotebookEditTool → fs/promises
 *   - AgentTool        -> in-process event loop (Local/Remote Agent Task)
 *   - WebFetchTool     -> globalThis.fetch (pure TS, no subprocess; domain allowlist/permissions built in)
 *
 * Explicitly disallow Bash / Glob / Grep: all three invoke external programs via ChildProcess under the hood
 * (bash shell, ripgrep), which breaks the semantic boundary of "pure JS execution."
 *
 * Lazy-loaded getter — same strategy as REPLTool/primitiveTools.ts:
 * There is a cyclic dependency chain in the tool registry (ScriptTool -> primitiveTools -> FileReadTool ->
 * tools registry -> ScriptTool); top-level const can hit a TDZ error
 * ("Cannot access before initialization"); constructing the array lazily at call time avoids it.
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
