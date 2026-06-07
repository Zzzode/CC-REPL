import { chmodSync, copyFileSync, existsSync, mkdirSync, writeFileSync } from 'fs'
import { dirname, resolve } from 'path'
import { spawnSync } from 'child_process'

const cmake = process.env.CMAKE ?? 'cmake'
const projectDir = resolve('cpp_migration')
const distDir = resolve('dist')
const binaryName = process.platform === 'win32' ? 'cc-repl.exe' : 'cc-repl'
const requestedPreset = process.env.CC_REPL_CMAKE_PRESET
const preset = selectCMakePreset(requestedPreset)
const builtBinary = resolve(projectDir, 'build', preset, 'bin', binaryName)
const distBinary = resolve(distDir, binaryName)
const compatibilityLauncher = resolve(distDir, 'cli.js')

function runQuiet(command, args = ['--version']) {
  const result = spawnSync(command, args, {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  if (result.error || result.status !== 0) return null
  return `${result.stdout ?? ''}${result.stderr ?? ''}`
}

function compilerSupportsRequiredCxx23(command) {
  const result = spawnSync(
    command,
    [
      '-std=c++23',
      '-x',
      'c++',
      '-',
      '-c',
      '-o',
      process.platform === 'win32' ? 'NUL' : `/tmp/cc-repl-cxx23-check-${process.pid}.o`,
    ],
    {
      input: [
        '#include <expected>',
        '#include <format>',
        '#include <stop_token>',
        '#include <thread>',
        '#include <string>',
        'int main() {',
        '  std::expected<int, std::string> value = 1;',
        '  std::string text = std::format("{}", *value);',
        '  std::jthread worker([](std::stop_token stop) { (void)stop; });',
        '  worker.request_stop();',
        '  return text == "1" ? 0 : 1;',
        '}',
        '',
      ].join('\n'),
      encoding: 'utf8',
      stdio: ['pipe', 'ignore', 'ignore'],
    },
  )
  return !result.error && result.status === 0
}

function localLlvm21Toolchain() {
  const home = process.env.HOME
  if (!home) return null
  return {
    cc: `${home}/.local/bin/clang-21-local`,
    cxx: `${home}/.local/bin/clang++-21-local`,
    scanDeps: `${home}/.local/bin/clang-scan-deps-21-local`,
  }
}

function linuxLlvm21PresetFor(presetName) {
  if (presetName === 'debug') return 'linux-debug'
  return 'linux-release'
}

function requireLinuxLlvm21Toolchain(presetName) {
  const toolchain = localLlvm21Toolchain()
  if (!toolchain) {
    console.error(
      `Linux CMake preset "${presetName}" requires HOME to locate the user-local LLVM 21 toolchain.`,
    )
    process.exit(1)
  }

  const missing = Object.values(toolchain).filter((path) => !existsSync(path))
  if (missing.length > 0) {
    console.error(
      [
        `Linux CMake preset "${presetName}" requires the project LLVM 21 toolchain.`,
        'Missing:',
        ...missing.map((path) => `  ${path}`),
        'Expected wrappers:',
        `  ${toolchain.cc}`,
        `  ${toolchain.cxx}`,
        `  ${toolchain.scanDeps}`,
      ].join('\n'),
    )
    process.exit(1)
  }

  const broken = Object.values(toolchain).filter((path) => runQuiet(path) === null)
  if (broken.length > 0) {
    console.error(
      [
        `Linux CMake preset "${presetName}" requires runnable LLVM 21 wrappers.`,
        'Not runnable:',
        ...broken.map((path) => `  ${path}`),
      ].join('\n'),
    )
    process.exit(1)
  }

  if (!compilerSupportsRequiredCxx23(toolchain.cxx)) {
    console.error(
      [
        `Linux CMake preset "${presetName}" requires LLVM 21 with complete C++23 library support.`,
        `The C++23 probe failed with ${toolchain.cxx}.`,
      ].join('\n'),
    )
    process.exit(1)
  }
}

function selectCMakePreset(explicitPreset) {
  if (process.platform === 'linux') {
    if (
      !explicitPreset ||
      explicitPreset === 'debug' ||
      explicitPreset === 'release' ||
      explicitPreset === 'linux-debug' ||
      explicitPreset === 'linux-release'
    ) {
      const selectedPreset =
        explicitPreset === 'linux-debug' || explicitPreset === 'linux-release'
          ? explicitPreset
          : linuxLlvm21PresetFor(explicitPreset ?? 'release')

      requireLinuxLlvm21Toolchain(selectedPreset)

      if (explicitPreset && explicitPreset !== selectedPreset) {
        console.warn(
          `Using Linux LLVM 21 CMake preset "${selectedPreset}" for "${explicitPreset}".`,
        )
      }

      return selectedPreset
    }

    return explicitPreset
  }

  if (explicitPreset) return explicitPreset

  if (
    process.platform === 'darwin' &&
    existsSync('/opt/homebrew/opt/llvm/bin/clang++') &&
    existsSync('/opt/homebrew/opt/llvm/bin/clang-scan-deps')
  ) {
    return 'clang-release'
  }

  return 'release'
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

run(cmake, ['--preset', preset])
run(cmake, ['--build', '--preset', preset, '--target', 'cc_repl'])

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
