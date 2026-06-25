import { chmodSync, copyFileSync, mkdirSync, writeFileSync } from 'fs'
import { dirname, resolve } from 'path'
import { spawnSync } from 'child_process'
import { availableParallelism, cpus } from 'os'

const cmake = process.env.CMAKE ?? 'cmake'
const projectDir = resolve('cpp_migration')
const distDir = resolve('dist')
const binaryName = process.platform === 'win32' ? 'cc-repl.exe' : 'cc-repl'
const requestedPreset = process.env.CC_REPL_CMAKE_PRESET
const preset = selectCMakePreset(requestedPreset)
const buildJobs = selectBuildJobs()
const builtBinary = resolve(projectDir, 'build', preset, 'bin', binaryName)
const distBinary = resolve(distDir, binaryName)
const compatibilityLauncher = resolve(distDir, 'cli.js')

function parseBuildJobs(value, name) {
  if (value == null || value === '') return null
  if (!/^[0-9]+$/.test(value)) {
    console.error(`${name} must be a non-negative integer, got ${JSON.stringify(value)}.`)
    process.exit(1)
  }

  const parsed = Number(value)
  if (!Number.isSafeInteger(parsed)) {
    console.error(`${name} is too large: ${JSON.stringify(value)}.`)
    process.exit(1)
  }
  if (parsed === 0) return null
  return parsed
}

function selectBuildJobs() {
  return (
    parseBuildJobs(process.env.CC_REPL_CMAKE_BUILD_JOBS, 'CC_REPL_CMAKE_BUILD_JOBS') ??
    parseBuildJobs(process.env.CMAKE_BUILD_PARALLEL_LEVEL, 'CMAKE_BUILD_PARALLEL_LEVEL') ??
    parseBuildJobs(process.env.CC_REPL_BUILD_JOBS, 'CC_REPL_BUILD_JOBS') ??
    availableParallelism?.() ??
    cpus().length ??
    4
  )
}

function selectCMakePreset(explicitPreset) {
  return explicitPreset ?? 'release'
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: projectDir,
    stdio: 'inherit',
    env: process.env,
    ...options,
  })
  if (result.error) throw result.error
  if (result.status !== 0) {
    process.exit(result.status ?? 1)
  }
}

mkdirSync(distDir, { recursive: true })

run(cmake, ['--preset', preset, `-DCC_REPL_BUILD_JOBS=${buildJobs}`])
run(cmake, ['--build', '--preset', preset, '--target', 'cc_repl', '--parallel', String(buildJobs)])

copyFileSync(builtBinary, distBinary)
chmodSync(distBinary, 0o755)

const wrapper = `#!/usr/bin/env bun
import { spawnSync } from 'child_process'
import { dirname, join } from 'path'
import { fileURLToPath } from 'url'

const bin = join(dirname(fileURLToPath(import.meta.url)), ${JSON.stringify(binaryName)})
const result = spawnSync(bin, process.argv.slice(2), { stdio: 'inherit', env: process.env })
if (result.error) throw result.error
process.exit(result.status ?? 1)
`

writeFileSync(compatibilityLauncher, wrapper, 'utf8')
chmodSync(compatibilityLauncher, 0o755)

console.log(`Built ${distBinary}`)
console.log(`Built ${compatibilityLauncher}`)
console.log(`CMake preset: ${preset}`)
console.log(`CMake build jobs: ${buildJobs}`)
