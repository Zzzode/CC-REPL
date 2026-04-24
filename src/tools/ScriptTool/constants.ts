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
export const MAX_TYPECHECK_DIAGNOSTICS = 20

/**
 * Error-code set used by validateInput.
 * Centralized here so callers can perform precise error dispatching and avoid magic numbers in business code.
 */
export const SCRIPT_VALIDATION_ERROR_CODE = {
  EMPTY_CODE: 1,
  TIMEOUT_EXCEEDED: 2,
} as const

export type ScriptValidationErrorCode =
  (typeof SCRIPT_VALIDATION_ERROR_CODE)[keyof typeof SCRIPT_VALIDATION_ERROR_CODE]

/**
 * Tool set hidden from the model-direct callable list when ScriptTool is enabled.
 *
 * Design motivation (same approach as REPLTool/REPL_ONLY_TOOLS):
 *   Force batch operations through ScriptTool to reduce multi-turn round trips.
 *
 * - Bash / Glob / Grep: these invoke external subprocesses (shell, ripgrep),
 *   and are intentionally not exposed in ScriptTool—direct use in VM context throws ReferenceError.
 * - Read / Write / Edit / NotebookEdit / Agent / WebFetch: pure TS implementations,
 *   hidden from direct external calls and only accessible via ScriptTool VM context.
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
