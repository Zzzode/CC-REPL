#!/usr/bin/env node

import { existsSync } from 'node:fs'
import { resolve } from 'node:path'
import { spawnSync } from 'node:child_process'

const repoRoot = resolve(new URL('..', import.meta.url).pathname)
const nativeBinary = resolve(repoRoot, 'dist', process.platform === 'win32' ? 'cc-repl.exe' : 'cc-repl')

function run(label, command, args, expectedText, options = {}) {
  const result = spawnSync(command, args, {
    cwd: repoRoot,
    encoding: 'utf8',
    env: options.env ?? process.env,
    input: options.input,
  })

  const output = `${result.stdout ?? ''}${result.stderr ?? ''}`
  if (result.error) throw result.error
  if (result.status !== 0) {
    process.stderr.write(output)
    throw new Error(`${label} failed with exit code ${result.status}`)
  }
  if (expectedText && !output.includes(expectedText)) {
    process.stderr.write(output)
    throw new Error(`${label} did not include expected text: ${expectedText}`)
  }
  console.log(`ok - ${label}`)
  return output
}

function normalizeInventoryName(name) {
  return name
    .replace(/Tool$/, '')
    .replaceAll('MCP', 'Mcp')
    .replaceAll('LSP', 'Lsp')
    .replaceAll('REPL', 'Repl')
    .replace(/([a-z0-9])([A-Z])/g, '$1_$2')
    .replace(/[^a-zA-Z0-9]+/g, '_')
    .toLowerCase()
    .replace(/^_+|_+$/g, '')
    .replace(/_tool$/, '')
    .replace(/^read$/, 'file_read')
    .replace(/^write$/, 'file_write')
    .replace(/^edit$/, 'file_edit')
    .replace(/^web_fetch$/, 'web_fetch')
    .replace(/^web_search$/, 'web_search')
}

if (!existsSync(nativeBinary)) {
  throw new Error(`Native binary not found at ${nativeBinary}. Run bun run build first.`)
}

run('native version', nativeBinary, ['--version'], 'cc-repl 1.0.0-cpp')
run('native help', nativeBinary, ['--help'], 'Usage: cc-repl [options]')
run('package start version', 'bun', ['run', 'start', '--', '--version'], 'cc-repl 1.0.0-cpp')
run('compat launcher version', 'bun', ['dist/cli.js', '--version'], 'cc-repl 1.0.0-cpp')
run('strict migration inventory', 'node', ['scripts/cpp-migration-inventory.mjs', '--strict'], 'CMake registered source/header entries')

const runtimeCommands = run('runtime command list', nativeBinary, ['--list-runtime-commands'], 'commit')
for (const command of ['/commit', '/mcp', '/review', '/skills', '/tasks', '/ant-trace', '/version', '/exit']) {
  if (!runtimeCommands.includes(command.slice(1))) {
    throw new Error(`runtime command list did not include ${command}`)
  }
}

const runtimeToolsOutput = run('runtime tool list', nativeBinary, ['--list-runtime-tools'], 'Bash')
const runtimeTools = runtimeToolsOutput.trim().split(/\r?\n/).filter(Boolean)
const normalizedTools = new Set(runtimeTools.map(normalizeInventoryName))
for (const tool of [
  'agent',
  'bash',
  'file_read',
  'file_write',
  'file_edit',
  'glob',
  'grep',
  'mcp',
  'lsp',
  'skill',
  'task_create',
  'script',
  'sleep',
  'web_fetch',
  'web_search',
]) {
  if (!normalizedTools.has(tool)) {
    throw new Error(`runtime tool list did not include ${tool}`)
  }
}
const simpleUiOutput = run(
  'simple ui command dispatch',
  nativeBinary,
  ['--simple-ui'],
  'Goodbye!',
  {
    env: { ...process.env, ANTHROPIC_API_KEY: process.env.ANTHROPIC_API_KEY || 'dummy' },
    input: '/help\n/commit\n/mcp list\n/ant-trace request-42\n/debug-tool-call {"name":"Bash","input":{"command":"pwd"}}\n/mock-limits 1\n/reset-limits all\n/extra-usage status\n/bridge status\n/onboarding status\n/version detail\n/exit\n',
  },
)
if (simpleUiOutput.includes('Unknown command: /commit')) {
  throw new Error('simple UI did not dispatch /commit through the migrated registry')
}
if (simpleUiOutput.includes('Unknown command: /mcp')) {
  throw new Error('simple UI did not dispatch /mcp through the migrated registry')
}
if (simpleUiOutput.includes('No dedicated local action')) {
  throw new Error('simple UI still dispatched a command through the surface fallback path')
}
for (const forbidden of ['command ready', 'workflow ready', 'adapter ready', 'workflow prepared']) {
  if (simpleUiOutput.includes(forbidden)) {
    throw new Error(`simple UI command dispatch still returned incomplete migration text: ${forbidden}`)
  }
}
for (const expected of [
  'ANT trace snapshot',
  'Tool-call payload valid',
  'Synthetic rate limit active',
  'Rate limit state reset: active=false',
  'Extra usage:',
  'Bridge status:',
  'Onboarding status',
]) {
  if (!simpleUiOutput.includes(expected)) {
    throw new Error(`simple UI command dispatch did not include expected runtime output: ${expected}`)
  }
}
if (!simpleUiOutput.includes('cc-repl 1.0.0-cpp (C++23')) {
  throw new Error('simple UI /version command did not report the native version')
}

const envWithoutApiKey = { ...process.env }
delete envWithoutApiKey.ANTHROPIC_API_KEY
const simpleUiNoKeyOutput = run(
  'simple ui slash commands without api key',
  nativeBinary,
  ['--simple-ui'],
  'Goodbye!',
  {
    env: envWithoutApiKey,
    input: '/version detail\n/exit\n',
  },
)
if (simpleUiNoKeyOutput.includes('ANTHROPIC_API_KEY environment variable is not set')) {
  throw new Error('simple UI still requires ANTHROPIC_API_KEY for slash-only command dispatch')
}
if (!simpleUiNoKeyOutput.includes('cc-repl 1.0.0-cpp (C++23')) {
  throw new Error('simple UI without api key did not dispatch /version')
}
