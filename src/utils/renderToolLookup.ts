import { findToolByName, type Tool, type Tools } from '../Tool.js'
import { getReplPrimitiveTools } from '../tools/REPLTool/primitiveTools.js'
import { getScriptPrimitiveTools } from '../tools/ScriptTool/primitiveTools.js'

/**
 * Resolve a tool for UI rendering, even when runtime tool filtering hides it
 * from direct model-callable tool lists (REPL/Script modes).
 */
export function findRenderableToolByName(
  tools: Tools | undefined,
  name: string,
): Tool | undefined {
  return (
    (tools ? findToolByName(tools, name) : undefined) ??
    findToolByName(getReplPrimitiveTools(), name) ??
    findToolByName(getScriptPrimitiveTools(), name)
  )
}
