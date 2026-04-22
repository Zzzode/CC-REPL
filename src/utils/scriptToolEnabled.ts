import { isEnvTruthy } from './envUtils.js'

/**
 * Centralized runtime check for ScriptTool availability.
 * Returns true when ENABLE_SCRIPT_TOOL env var is set to a truthy value.
 */
export function isScriptToolEnabled(): boolean {
  return isEnvTruthy(process.env.ENABLE_SCRIPT_TOOL)
}

/**
 * Centralized runtime check for disabling BashTool.
 * Returns true when DISABLE_BASH_TOOL env var is set to a truthy value,
 * OR when ScriptTool is enabled (ScriptTool replaces Bash).
 */
export function isBashToolDisabled(): boolean {
  return isEnvTruthy(process.env.DISABLE_BASH_TOOL) || isScriptToolEnabled()
}
