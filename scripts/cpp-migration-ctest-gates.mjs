#!/usr/bin/env node

import { existsSync } from 'node:fs'
import { resolve } from 'node:path'
import { spawnSync } from 'node:child_process'

const repoRoot = resolve(new URL('..', import.meta.url).pathname)
const cmake = process.env.CMAKE ?? 'cmake'

const testTargets = [
  'test_bridge',
  'test_services',
  'test_state',
  'test_tools',
  'test_ui',
]

const gates = [
  {
    label: 'P0 bridge/headless remote lifecycle',
    regex: 'Bridge(Api|WorkSecret|Daemon)|SessionIngress\\.|RemoteSession\\.',
  },
  {
    label: 'P1 direct-connect permission protocol',
    regex: 'ServerMain\\.DirectConnect',
  },
  {
    label: 'P1 sub-agent permission context',
    regex: 'Tools\\.AgentToolLivePermissionHook|Tools\\.AgentToolBackgroundAgentPreservesLivePermissionHook',
  },
  {
    label: 'P1 SendMessage/team/swarm protocol',
    regex: 'Tools\\.RuntimeSendMessage|Tools\\.RuntimeTeamCreate|Tools\\.TeamStore|SpawnMultiAgent\\.',
  },
  {
    label: 'P1 MCP auth and remote behavior',
    regex: 'Mcp(Auth|ConnectionManager)|Tools\\.McpAuth|McpClient\\.|McpTool\\.|IdeIntegration\\.',
  },
  {
    label: 'P1 session/compaction/context semantics',
    regex: 'SessionHistory\\.|QueryEngine\\.(Snip|SerializesTaskBudget|Compact|Restore|RepeatedCompact|AutoCompact|ReactiveCompact|TimeBasedMicrocompact)|ApiMicrocompact\\.',
  },
  {
    label: 'P1 UI runtime behavior',
    regex: 'Terminal\\.StatusBar|Components\\.RenderPermissionPrompt|AppRuntime\\.',
  },
  {
    label: 'P2 platform and external integrations',
    regex: 'GitHubUtils\\.|LspConfig\\.|CcrClient\\.|Tools\\.(WebBrowserTool|RuntimeWebBrowser|PowerShellTool|PowerShellEncodingHandler|PowerShellEncodedCommand|RuntimePowerShell|RuntimeComputerUse|NativeComputerUse|ComputerUseManager)|Plugin(Identifier|Loader|DependencyResolver|Marketplace|MarketplaceRules|Versioning)\\.|Tools\\.McpRuntimeLoadsPlugin',
  },
]

function candidateBuildDirs() {
  const dirs = []
  if (process.env.CC_REPL_CMAKE_BUILD_DIR) {
    dirs.push(resolve(repoRoot, process.env.CC_REPL_CMAKE_BUILD_DIR))
  }
  if (process.env.CC_REPL_CMAKE_PRESET) {
    dirs.push(resolve(repoRoot, 'cpp_migration', 'build', process.env.CC_REPL_CMAKE_PRESET))
  }
  dirs.push(
    resolve(repoRoot, 'cpp_migration', 'build', 'linux-release'),
    resolve(repoRoot, 'cpp_migration', 'build', 'clang-release'),
    resolve(repoRoot, 'cpp_migration', 'build', 'release'),
    resolve(repoRoot, 'cpp_migration', 'build', 'debug'),
  )
  return [...new Set(dirs)]
}

function findBuildDir() {
  for (const dir of candidateBuildDirs()) {
    if (existsSync(resolve(dir, 'CMakeCache.txt'))) return dir
  }
  throw new Error([
    'No configured CMake build directory found.',
    'Run `bun run build` first, or set CC_REPL_CMAKE_BUILD_DIR.',
  ].join('\n'))
}

function run(label, command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: repoRoot,
    stdio: 'inherit',
    env: process.env,
    ...options,
  })
  if (result.error) throw result.error
  if (result.status !== 0) {
    throw new Error(`${label} failed with exit code ${result.status}`)
  }
}

function runCapture(label, command, args) {
  const result = spawnSync(command, args, {
    cwd: repoRoot,
    encoding: 'utf8',
    env: process.env,
  })
  if (result.error) throw result.error
  if (result.status !== 0) {
    if (result.stdout) process.stdout.write(result.stdout)
    if (result.stderr) process.stderr.write(result.stderr)
    throw new Error(`${label} failed with exit code ${result.status}`)
  }
  return `${result.stdout ?? ''}${result.stderr ?? ''}`
}

function countListedTests(ctestOutput) {
  return ctestOutput.match(/^\s*Test\s+#\d+:/gm)?.length ?? 0
}

const buildDir = findBuildDir()

run(
  'build C++ migration test targets',
  cmake,
  ['--build', buildDir, '--target', ...testTargets],
)

for (const gate of gates) {
  const listedTests = runCapture(
    `${gate.label} discovery`,
    'ctest',
    ['--test-dir', buildDir, '-N', '-R', gate.regex],
  )
  const testCount = countListedTests(listedTests)
  if (testCount === 0) {
    throw new Error(`${gate.label} matched no CTest tests with regex: ${gate.regex}`)
  }

  run(
    gate.label,
    'ctest',
    ['--test-dir', buildDir, '-R', gate.regex, '--output-on-failure'],
  )
  console.log(`ok - ${gate.label} (${testCount} tests)`)
}
