#!/usr/bin/env node

import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs'
import path from 'node:path'
import process from 'node:process'

const repoRoot = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..')
const tsRoot = path.join(repoRoot, 'src')
const cppRoot = path.join(repoRoot, 'cpp_migration', 'src')
const cmakeFile = path.join(cppRoot, 'CMakeLists.txt')

const tsExtensions = new Set(['.ts', '.tsx', '.js', '.jsx'])
const cppExtensions = new Set(['.cppm', '.cpp', '.h', '.hpp'])
const ignoredDirs = new Set(['.git', 'node_modules', 'dist', 'build', '.cache'])
const blockingMarkerRegex = /\bTODO\b|\bFIXME\b|Not implemented|not implemented|mock success|For now, return mock|TODO:\s*Implement/
const debtMarkerRegex = /\bstubbed\b|In production:/i
const placeholderRegex = /placeholder/i
const allowedPlaceholderFiles = new Set([
  'cli/handlers/template_jobs.cppm',
  'hooks/render_placeholder.cppm',
  'skills/lorem_ipsum.cppm',
  'ui/components/structured_diff.cppm',
  'ui/components/text_input_widget.cppm',
  'ui/dialogs/wizard_dialog.cppm',
  'ui/messages/message_image.cppm',
  'ui/messages/message_redacted_thinking.cppm',
  'ui/screens/repl_screen.cppm',
  'utils/argument_substitution.cppm',
  'utils/hooks_registry.cppm',
  'utils/mcp_validation.cppm',
  'utils/message_processing.cppm',
])

function classifyMarkerLine(relativePath, line) {
  if (blockingMarkerRegex.test(line)) return 'blocking'
  if (debtMarkerRegex.test(line)) return 'nonBlocking'
  if (!placeholderRegex.test(line)) return null

  const normalized = line.toLowerCase()
  const allowedPlaceholderPatterns = [
    'render_placeholder_shimmer',
    'placeholder.empty()',
    'placeholder.size()',
    'placeholder[i]',
    'std::string placeholder',
    'std::optional<std::string> placeholder',
    'options_.placeholder',
    'summary placeholder',
    'the summary placeholder',
    '{{' ,
  ]
  if (allowedPlaceholderPatterns.some(pattern => normalized.includes(pattern))) return 'nonBlocking'

  if (relativePath === 'ui/prompt/shimmer.cppm') return 'nonBlocking'
  if (relativePath === 'tools/synthetic_output_tool.cppm') return 'nonBlocking'
  if (relativePath === 'ui/components/text_input.cppm') return 'nonBlocking'
  if (relativePath === 'tools/ask_user_tool.cppm') return 'nonBlocking'
  if (relativePath === 'ui/app.cppm') return 'nonBlocking'
  if (relativePath === 'types/logs.cppm') return 'nonBlocking'
  if (allowedPlaceholderFiles.has(relativePath)) return 'nonBlocking'

  return 'blocking'
}

const args = new Set(process.argv.slice(2))
const jsonOnly = args.has('--json')
const failOnOrphans = args.has('--fail-on-orphans') || args.has('--strict')
const failOnMissingCmake = args.has('--fail-on-missing-cmake') || args.has('--strict')
const failOnCoreMarkers = args.has('--fail-on-core-markers') || args.has('--strict')

function walk(root, predicate) {
  if (!existsSync(root)) return []
  const out = []
  const stack = [root]
  while (stack.length > 0) {
    const current = stack.pop()
    for (const entry of readdirSync(current)) {
      if (ignoredDirs.has(entry)) continue
      const full = path.join(current, entry)
      const stat = statSync(full)
      if (stat.isDirectory()) {
        stack.push(full)
      } else if (!predicate || predicate(full)) {
        out.push(full)
      }
    }
  }
  return out.sort()
}

function rel(file, root) {
  return path.relative(root, file).split(path.sep).join('/')
}

function topLevel(relativePath) {
  const parts = relativePath.split('/')
  return parts.length > 1 ? parts[0] : '(root)'
}

function inc(map, key, delta = 1) {
  map[key] = (map[key] ?? 0) + delta
}

function listSourceFiles(root, extensions) {
  return walk(root, file => extensions.has(path.extname(file))).map(file => rel(file, root))
}

function countByTop(files) {
  const counts = {}
  for (const file of files) inc(counts, topLevel(file))
  return Object.fromEntries(Object.entries(counts).sort(([a], [b]) => a.localeCompare(b)))
}

function collectMarkers(files) {
    const byFile = []
    const byTop = {}
    const blockingByFile = []
    const blockingByTop = {}
    const nonBlockingByFile = []
    let total = 0
    let blockingTotal = 0
    let nonBlockingTotal = 0
    for (const relativePath of files) {
      const absolute = path.join(cppRoot, relativePath)
      const text = readFileSync(absolute, 'utf8')
      const lines = text.split(/\r?\n/)
      const entries = []
      const blockingEntries = []
      const nonBlockingEntries = []
      for (let i = 0; i < lines.length; i++) {
      const kind = classifyMarkerLine(relativePath, lines[i])
      if (kind) {
        const entry = { line: i + 1, text: lines[i].trim(), kind }
        entries.push(entry)
        if (kind === 'blocking') blockingEntries.push(entry)
        else nonBlockingEntries.push(entry)
      }
    }
    if (entries.length === 0) continue
    total += entries.length
    blockingTotal += blockingEntries.length
    nonBlockingTotal += nonBlockingEntries.length
    inc(byTop, topLevel(relativePath), entries.length)
    if (blockingEntries.length) inc(blockingByTop, topLevel(relativePath), blockingEntries.length)
    byFile.push({ file: relativePath, count: entries.length, entries })
    if (blockingEntries.length) {
      blockingByFile.push({ file: relativePath, count: blockingEntries.length, entries: blockingEntries })
    }
    if (nonBlockingEntries.length) {
      nonBlockingByFile.push({ file: relativePath, count: nonBlockingEntries.length, entries: nonBlockingEntries })
    }
  }
  byFile.sort((a, b) => b.count - a.count || a.file.localeCompare(b.file))
  blockingByFile.sort((a, b) => b.count - a.count || a.file.localeCompare(b.file))
  nonBlockingByFile.sort((a, b) => b.count - a.count || a.file.localeCompare(b.file))
  return {
    total,
    blockingTotal,
    nonBlockingTotal,
    byTop: Object.fromEntries(Object.entries(byTop).sort((a, b) => b[1] - a[1])),
    blockingByTop: Object.fromEntries(Object.entries(blockingByTop).sort((a, b) => b[1] - a[1])),
    byFile,
    blockingByFile,
    nonBlockingByFile,
  }
}

function collectCmakeRegisteredFiles() {
  if (!existsSync(cmakeFile)) return new Set()
  const text = readFileSync(cmakeFile, 'utf8')
  const registered = new Set()
  const re = /(?:^|\s)((?:[A-Za-z0-9_.-]+\/)*[A-Za-z0-9_.-]+\.(?:cppm|cpp|hpp|h))/gm
  for (const match of text.matchAll(re)) {
    registered.add(match[1])
  }
  return registered
}

const tsFiles = listSourceFiles(tsRoot, tsExtensions)
const cppFiles = listSourceFiles(cppRoot, cppExtensions)
const cppModuleFiles = cppFiles.filter(file => file.endsWith('.cppm'))
const cppImplementationFiles = cppFiles.filter(file => file.endsWith('.cpp'))
const cppHeaderFiles = cppFiles.filter(file => file.endsWith('.h') || file.endsWith('.hpp'))
const registered = collectCmakeRegisteredFiles()
const cppCompilableFiles = cppFiles.filter(file => file.endsWith('.cppm') || file.endsWith('.cpp'))
const unregisteredCompilableFiles = cppCompilableFiles.filter(file => !registered.has(file))
const registeredMissingFiles = [...registered].filter(file => !existsSync(path.join(cppRoot, file))).sort()

const markers = collectMarkers(cppFiles)
const topLevelCoverage = {}
const tsByTop = countByTop(tsFiles)
const cppByTop = countByTop(cppFiles)
for (const key of [...new Set([...Object.keys(tsByTop), ...Object.keys(cppByTop)])].sort()) {
  topLevelCoverage[key] = {
    ts: tsByTop[key] ?? 0,
    cpp: cppByTop[key] ?? 0,
    ratio: tsByTop[key] ? Number(((cppByTop[key] ?? 0) / tsByTop[key]).toFixed(2)) : null,
  }
}

const report = {
  generatedAt: new Date().toISOString(),
  roots: {
    repoRoot,
    tsRoot: path.relative(repoRoot, tsRoot),
    cppRoot: path.relative(repoRoot, cppRoot),
  },
  totals: {
    tsSourceFiles: tsFiles.length,
    cppSourceFiles: cppFiles.length,
    cppModuleFiles: cppModuleFiles.length,
    cppImplementationFiles: cppImplementationFiles.length,
    cppHeaderFiles: cppHeaderFiles.length,
  },
  topLevelCoverage,
  cmake: {
    registeredCount: registered.size,
    unregisteredCompilableFiles,
    registeredMissingFiles,
  },
  markers,
}

if (jsonOnly) {
  console.log(JSON.stringify(report, null, 2))
} else {
  console.log('CC-REPL C++ migration inventory')
  console.log('==================================')
  console.log(`Generated: ${report.generatedAt}`)
  console.log(`TS source files: ${report.totals.tsSourceFiles}`)
  console.log(`C++ source files: ${report.totals.cppSourceFiles} (${report.totals.cppModuleFiles} .cppm, ${report.totals.cppImplementationFiles} .cpp, ${report.totals.cppHeaderFiles} headers)`)
  console.log('')
  console.log(`CMake registered source/header entries: ${report.cmake.registeredCount}`)
  console.log(`Unregistered C++ compilable files: ${report.cmake.unregisteredCompilableFiles.length}`)
  if (report.cmake.unregisteredCompilableFiles.length) {
    for (const file of report.cmake.unregisteredCompilableFiles.slice(0, 30)) console.log(`  - ${file}`)
    if (report.cmake.unregisteredCompilableFiles.length > 30) console.log(`  ... ${report.cmake.unregisteredCompilableFiles.length - 30} more`)
  }
  console.log(`Registered but missing files: ${report.cmake.registeredMissingFiles.length}`)
  if (report.cmake.registeredMissingFiles.length) {
    for (const file of report.cmake.registeredMissingFiles) console.log(`  - ${file}`)
  }
  console.log('')
  console.log(`Migration markers: ${report.markers.total} occurrences across ${report.markers.byFile.length} files`)
  console.log(`  Blocking: ${report.markers.blockingTotal}`)
  console.log(`  Non-blocking placeholders: ${report.markers.nonBlockingTotal}`)
  for (const [dir, count] of Object.entries(report.markers.byTop).slice(0, 12)) {
    console.log(`  ${dir}: ${count}`)
  }
}

let failed = false
if (failOnOrphans && unregisteredCompilableFiles.length > 0) failed = true
if (failOnMissingCmake && registeredMissingFiles.length > 0) failed = true
if (failOnCoreMarkers && markers.blockingTotal > 0) failed = true

if (failed) process.exitCode = 1
