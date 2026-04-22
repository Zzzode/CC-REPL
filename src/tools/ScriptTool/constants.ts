import { AGENT_TOOL_NAME } from '../AgentTool/constants.js'
import { BASH_TOOL_NAME } from '../BashTool/toolName.js'
import { FILE_EDIT_TOOL_NAME } from '../FileEditTool/constants.js'
import { FILE_READ_TOOL_NAME } from '../FileReadTool/prompt.js'
import { FILE_WRITE_TOOL_NAME } from '../FileWriteTool/prompt.js'
import { GLOB_TOOL_NAME } from '../GlobTool/prompt.js'
import { GREP_TOOL_NAME } from '../GrepTool/prompt.js'
import { NOTEBOOK_EDIT_TOOL_NAME } from '../NotebookEditTool/constants.js'
import { WEB_FETCH_TOOL_NAME } from '../WebFetchTool/prompt.js'

export const SCRIPT_TOOL_NAME = 'Script'

export const DEFAULT_TIMEOUT_MS = 30000
export const MAX_TIMEOUT_MS = 120000

export const MAX_OUTPUT_SIZE = 100 * 1024 // 100KB

/**
 * validateInput 使用的错误码集合。
 * 集中到此处以便调用方做精确的错误分派，避免在业务代码里使用魔法数字。
 */
export const SCRIPT_VALIDATION_ERROR_CODE = {
  EMPTY_CODE: 1,
  TIMEOUT_EXCEEDED: 2,
} as const

export type ScriptValidationErrorCode =
  (typeof SCRIPT_VALIDATION_ERROR_CODE)[keyof typeof SCRIPT_VALIDATION_ERROR_CODE]

/**
 * 当 ScriptTool 启用时，从模型可直接调用列表中隐藏的工具集合。
 *
 * 设计动机（与 REPLTool/REPL_ONLY_TOOLS 同款思路）：
 *   强制模型走 ScriptTool 做批量操作，减少多轮 round-trip。
 *
 * - Bash / Glob / Grep：底层调外部子进程（shell、ripgrep），
 *   ScriptTool 明确拒绝暴露——在 VM 上下文中直接使用会抛 ReferenceError。
 * - Read / Write / Edit / NotebookEdit / Agent / WebFetch：纯 TS 实现，
 *   隐藏对外直接调用后，仅能通过 ScriptTool 的 VM 上下文访问。
 */
export const SCRIPT_HIDDEN_TOOLS = new Set<string>([
  BASH_TOOL_NAME,
  GLOB_TOOL_NAME,
  GREP_TOOL_NAME,
  FILE_READ_TOOL_NAME,
  FILE_WRITE_TOOL_NAME,
  FILE_EDIT_TOOL_NAME,
  NOTEBOOK_EDIT_TOOL_NAME,
  AGENT_TOOL_NAME,
  WEB_FETCH_TOOL_NAME,
])
