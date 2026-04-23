import { MAX_TIMEOUT_MS } from './constants.js'

export const DESCRIPTION = `Execute TypeScript/JavaScript code that composes pure-TS primitive tools (Read, Write, Edit, NotebookEdit, Agent, WebFetch) in a single round-trip. Use this instead of calling those tools individually for multi-step file or HTTP workflows.`

export function getPrompt(): string {
  return `${DESCRIPTION}

## Execution model

Your code runs inside a lightweight async sandbox (Bun.Transpiler + AsyncFunction).
Inside the sandbox, the following tools are exposed as **async functions** that
delegate to the real in-process Tool objects — same input schemas, same permission
checks, same side effects as if you had called them directly:

| Binding         | Backing tool        | Notes                                       |
|-----------------|---------------------|---------------------------------------------|
| \`Read\`        | FileReadTool        | Pure Node \`fs\` reader                     |
| \`Write\`       | FileWriteTool       | Pure Node \`fs\` writer                     |
| \`Edit\`        | FileEditTool        | Exact-match string replace                  |
| \`NotebookEdit\`| NotebookEditTool    | Jupyter notebook cells                      |
| \`Agent\`       | AgentTool           | Spawn sub-agent in-process                  |
| \`WebFetch\`    | WebFetchTool        | HTTP via \`globalThis.fetch\` (whitelisted) |

Each call returns a **script-friendly value** (not the raw internal data object):

| Binding         | Return type         | Details                                             |
|-----------------|---------------------|-----------------------------------------------------|
| \`Read\`        | \`string\`          | File content as text (images/PDFs return raw object) |
| \`Write\`       | \`string\`          | Confirmation message (e.g. \`"File created …"\`)    |
| \`Edit\`        | \`string\`          | Confirmation message                                |
| \`NotebookEdit\`| \`string\`          | Confirmation message (throws on error)              |
| \`Agent\`       | \`string\`          | Subagent text output (all text blocks joined)       |
| \`WebFetch\`    | \`string\`          | Processed/summarized content from the URL           |

Errors become regular JS exceptions — use try/catch.

## utils namespace

The \`utils\` object exposes side-effect-free helpers:

- \`utils.cwd\` — current working directory (read-only string getter)
- \`utils.sleep(ms)\` — Promise-based sleep, **cancelled automatically** when the
  Script is aborted or times out (rejects with an Error instead of resolving).

## Banned inside Script

The following tools are **not injected** into the sandbox. Referencing them will
throw \`ReferenceError\`:

  - \`Bash\`  — spawns a real shell subprocess (violates the pure-TS boundary)
  - \`Glob\`  — spawns ripgrep as a subprocess
  - \`Grep\`  — spawns ripgrep as a subprocess

Static \`import\`/\`export\` statements are not allowed (code runs inside an async
function body). Use the injected bindings above instead.

## Conventions

- Top-level \`await\` and \`return\` are supported (code is wrapped in an async fn).
- \`console.log/info\` → captured into \`stdout\`; \`console.error/warn\` → \`stderr\`.
- Use \`return\` to emit a structured result back to the model.

## Examples

\`\`\`typescript
// Read a file, transform it, write it back
const raw = await Read({ file_path: '/abs/path/to/pkg.json' })
const pkg = JSON.parse(raw)
pkg.version = '1.2.3'
await Write({ file_path: '/abs/path/to/pkg.json', content: JSON.stringify(pkg, null, 2) })
return { version: pkg.version, cwd: utils.cwd }
\`\`\`

\`\`\`typescript
// Batch-edit several files in one round-trip
const files = [
  '/abs/a.ts',
  '/abs/b.ts',
  '/abs/c.ts',
]
for (const file of files) {
  await Edit({
    file_path: file,
    old_string: 'oldName',
    new_string: 'newName',
  })
}
return { edited: files.length }
\`\`\`

\`\`\`typescript
// Fetch a URL and summarize — respects WebFetchTool's domain whitelist
const result = await WebFetch({
  url: 'https://example.com/docs',
  prompt: 'Extract the API endpoints',
})
return { endpoints: result }
\`\`\`

\`\`\`typescript
// Poll an endpoint with abort-safe sleep
for (let i = 0; i < 5; i++) {
  const res = await WebFetch({ url: 'https://example.com/status', prompt: 'ready?' })
  if (res.includes('ok')) return { ready: true, attempts: i + 1 }
  await utils.sleep(1000)
}
return { ready: false }
\`\`\`

## Limits

- Max execution time: ${MAX_TIMEOUT_MS / 1000}s (override via \`timeout_ms\`)
- Max captured output size: 100KB (stdout + stderr each, truncated middle)
`
}
