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
run('strict migration inventory', 'node', ['scripts/cpp-migration-inventory.mjs', '--strict'], 'Commands: 100/100 migrated')

const runtimeCommands = run('runtime command list', nativeBinary, ['--list-runtime-commands'], 'commit')
for (const command of ['/commit', '/mcp', '/review', '/skills', '/tasks', '/exit']) {
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
if (runtimeTools.length < 45) {
  throw new Error(`runtime tool list exposed only ${runtimeTools.length} tools`)
}

const simpleUiOutput = run(
  'simple ui command dispatch',
  nativeBinary,
  ['--simple-ui'],
  'Goodbye!',
  {
    env: { ...process.env, ANTHROPIC_API_KEY: process.env.ANTHROPIC_API_KEY || 'dummy' },
    input: '/help\n/commit\n/mcp list\n/exit\n',
  },
)
if (simpleUiOutput.includes('Unknown command: /commit')) {
  throw new Error('simple UI did not dispatch /commit through the migrated registry')
}
if (simpleUiOutput.includes('Unknown command: /mcp')) {
  throw new Error('simple UI did not dispatch /mcp through the migrated registry')
}
