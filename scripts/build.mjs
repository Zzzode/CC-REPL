import { chmodSync, copyFileSync, mkdirSync, writeFileSync } from 'fs'
import { dirname, resolve } from 'path'
import { spawnSync } from 'child_process'

const preset = process.env.CC_REPL_CMAKE_PRESET ?? 'clang-release'
const cmake = process.env.CMAKE ?? 'cmake'
const projectDir = resolve('cpp_migration')
const distDir = resolve('dist')
const binaryName = process.platform === 'win32' ? 'cc-repl.exe' : 'cc-repl'
const builtBinary = resolve(projectDir, 'build', preset, 'bin', binaryName)
const distBinary = resolve(distDir, binaryName)
const compatibilityLauncher = resolve(distDir, 'cli.js')

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
