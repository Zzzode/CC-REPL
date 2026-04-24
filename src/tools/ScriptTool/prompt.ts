import { MAX_OUTPUT_SIZE, MAX_TIMEOUT_MS } from './constants.js'

export const DESCRIPTION =
  'Primary TypeScript execution tool for file operations, web retrieval, and multi-step orchestration in a sandboxed async environment.'

type BindingDoc = {
  binding: string
  backingTool: string
  notes: string
  returnType: string
  returnDetails: string
}

const TOOL_BINDINGS: BindingDoc[] = [
  {
    binding: 'Read',
    backingTool: 'FileReadTool',
    notes: 'Pure Node fs reader',
    returnType: 'string | object',
    returnDetails:
      'Text files return content string; non-text responses (image/pdf/notebook) return structured objects.',
  },
  {
    binding: 'Write',
    backingTool: 'FileWriteTool',
    notes: 'Pure Node fs writer',
    returnType: 'string',
    returnDetails:
      'Confirmation message, e.g. "File created successfully at: ..." or "The file ... has been updated successfully."',
  },
  {
    binding: 'Edit',
    backingTool: 'FileEditTool',
    notes: 'Exact-match string replace',
    returnType: 'string',
    returnDetails: 'Confirmation message.',
  },
  {
    binding: 'NotebookEdit',
    backingTool: 'NotebookEditTool',
    notes: 'Jupyter notebook cell operations',
    returnType: 'string',
    returnDetails:
      'Returns action message (Inserted/Deleted/Updated cell ...); throws when tool reports an error.',
  },
  {
    binding: 'Agent',
    backingTool: 'AgentTool',
    notes: 'Spawn sub-agent in-process',
    returnType: 'string',
    returnDetails: 'All text blocks joined with newlines.',
  },
  {
    binding: 'WebFetch',
    backingTool: 'WebFetchTool',
    notes: 'HTTP via globalThis.fetch (whitelisted)',
    returnType: 'string',
    returnDetails: 'Processed/summarized content from the URL.',
  },
]

function escapeTableCell(value: string): string {
  return value.replaceAll('|', '\\|')
}

function renderBindingTable(): string {
  const header = [
    '| Binding | Backing tool | Notes |',
    '|---|---|---|',
  ]
  const rows = TOOL_BINDINGS.map(
    binding =>
      `| \`${escapeTableCell(binding.binding)}\` | ${escapeTableCell(binding.backingTool)} | ${escapeTableCell(binding.notes)} |`,
  )
  return [...header, ...rows].join('\n')
}

function renderReturnTable(): string {
  const header = [
    '| Binding | Return type | Details |',
    '|---|---|---|',
  ]
  const rows = TOOL_BINDINGS.map(
    binding =>
      `| \`${escapeTableCell(binding.binding)}\` | \`${escapeTableCell(binding.returnType)}\` | ${escapeTableCell(binding.returnDetails)} |`,
  )
  return [...header, ...rows].join('\n')
}

export function getPrompt(): string {
  return `${DESCRIPTION}

## Capabilities

When ScriptTool is enabled, these operations are routed through Script:

- File operations (Read / Write / Edit / NotebookEdit)
- Web content retrieval for known URLs (WebFetch)
- Multi-step orchestration with conditional logic and loops
- Sub-agent orchestration (Agent)
- Data processing in TypeScript (JavaScript is supported as a subset)

Even a single Read/Write/Edit/WebFetch call must be done through Script.

## Execution model

Code runs in a lightweight async sandbox (Bun.Transpiler + AsyncFunction).
Execution pipeline:

1. Syntax transpilation (TypeScript -> JavaScript)
2. Type checking (error-level diagnostics block execution)
3. Runtime execution in sandbox context

Inside the sandbox, each injected binding is an async function that delegates to
its in-process tool with the same input schema, permission checks, and side effects.

${renderBindingTable()}

Each binding returns a script-friendly value instead of raw internal tool data:

${renderReturnTable()}

### Agent binding input

\`Agent()\` delegates work to a sub-agent that has access to the full tool set.

| Parameter | Required | Description |
|---|---|---|
| \`description\` | **yes** | Short (3-5 word) task description |
| \`prompt\` | **yes** | Detailed task instructions for the sub-agent |
| \`subagent_type\` | no | Agent type (defaults to \`'general-purpose'\`) |

Available built-in agent types:
- \`Explore\`: Fast agent specialized for exploring codebases. Use this when you need to quickly find files by patterns, search code for keywords, or answer questions about the codebase. The Explore agent has access to Bash, Glob, Grep, and Read tools.
- \`Plan\`: Use this when you need to create a detailed plan for a complex task.
- \`General-purpose\`: Default agent for general tasks. Use this when no specialized agent fits your needs.

When to use each agent type in Script:
- **Explore**: Use for file searches, code exploration, and codebase analysis. Since Bash/Glob/Grep are unavailable in Script, Explore is the primary way to perform these operations.
- **Plan**: Use for complex multi-step tasks that require planning before execution.
- **General-purpose**: Use for general tasks that don't fit the above categories.

Tool and runtime failures are regular JavaScript exceptions (use try/catch).

## utils namespace

The \`utils\` object exposes side-effect-free helpers:

- \`utils.cwd\` — current working directory (read-only string getter)
- \`utils.sleep(ms)\` — Promise-based sleep that is cancelled on abort/timeout
  (rejects with an Error instead of resolving)

## Unsupported in Script

These symbols are not injected into the sandbox. Referencing them throws
\`ReferenceError\`:

- \`Bash\`
- \`Glob\`
- \`Grep\`

To perform file search, shell commands, or other operations requiring these tools,
delegate to a sub-agent via \`Agent({ ... })\` — the sub-agent has access to the
full tool set including Bash and Grep.

Static \`import\` / \`export\` statements are not allowed (code runs inside an
async function body). Use injected bindings instead.

## Conventions

- Top-level \`await\` and \`return\` are supported
- \`console.log/info\` is captured into stdout
- \`console.warn/error\` is captured into stderr
- Use \`return\` to emit structured output back to the model

## Examples

\`\`\`typescript
// Read -> transform -> write
const raw = await Read({ file_path: '/abs/path/to/pkg.json' })
const pkg = JSON.parse(raw)
pkg.version = '1.2.3'
await Write({
  file_path: '/abs/path/to/pkg.json',
  content: JSON.stringify(pkg, null, 2),
})
return { version: pkg.version, cwd: utils.cwd }
\`\`\`

\`\`\`typescript
// Batch edit with loop + conditional logic
const files = ['/abs/a.ts', '/abs/b.ts', '/abs/c.ts']
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
// Fetch known URL content
const summary = await WebFetch({
  url: 'https://example.com/docs',
  prompt: 'Extract API endpoints and auth requirements',
})
return { summary }
\`\`\`

\`\`\`typescript
// Delegate search to Explore agent (Bash/Grep are unavailable in Script)
const result = await Agent({
  description: 'Find TODO comments',
  subagent_type: 'Explore',
  prompt: \`Find all lines containing "TODO" in .ts files under \${utils.cwd}/src. Return file paths and line numbers.\`,
})
return result
\`\`\`

## Limits

- Max execution time: ${MAX_TIMEOUT_MS / 1000}s (override via \`timeout_ms\`)
- Max captured output size: ${Math.floor(MAX_OUTPUT_SIZE / 1024)}KB (stdout and stderr each)
`
}
