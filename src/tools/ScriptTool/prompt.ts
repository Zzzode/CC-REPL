import { shouldInjectAgentListInMessages } from '../AgentTool/prompt.js'
import { MAX_OUTPUT_SIZE, MAX_TIMEOUT_MS } from './constants.js'

export const DESCRIPTION =
  'Primary TypeScript execution tool for file operations, web retrieval, and multi-step orchestration in a sandboxed async environment.'

type BindingDoc = {
  binding: string
  backingTool: string
  notes: string
}

type PromptAgent = {
  agentType: string
  whenToUse: string
}

const TOOL_BINDINGS: BindingDoc[] = [
  {
    binding: 'Read',
    backingTool: 'FileReadTool',
    notes: 'returns string for text; object for image/pdf/notebook',
  },
  { binding: 'Write', backingTool: 'FileWriteTool', notes: 'returns confirmation string' },
  { binding: 'Edit', backingTool: 'FileEditTool', notes: 'exact-match replace; returns confirmation string' },
  {
    binding: 'NotebookEdit',
    backingTool: 'NotebookEditTool',
    notes: 'Jupyter cell ops; returns action message',
  },
  { binding: 'Agent', backingTool: 'AgentTool', notes: 'spawn sub-agent; returns joined text' },
  {
    binding: 'WebFetch',
    backingTool: 'WebFetchTool',
    notes: 'HTTP fetch (whitelisted); returns processed content',
  },
]

function escapeTableCell(value: string): string {
  return value.replaceAll('|', '\\|')
}

function renderBindingTable(): string {
  const header = ['| Binding | Backing tool | Notes |', '|---|---|---|']
  const rows = TOOL_BINDINGS.map(
    b =>
      `| \`${escapeTableCell(b.binding)}\` | ${escapeTableCell(b.backingTool)} | ${escapeTableCell(b.notes)} |`,
  )
  return [...header, ...rows].join('\n')
}

function canonicalizeAgentType(agentType: string): string {
  return agentType.trim().toLowerCase().replaceAll('_', '-').replaceAll(' ', '-')
}

function pickSearchAgentType(agents: readonly PromptAgent[]): string {
  for (const agent of agents) {
    const canonical = canonicalizeAgentType(agent.agentType)
    if (
      canonical === 'explore' ||
      canonical === 'explorer' ||
      canonical === 'code-explorer' ||
      canonical.endsWith(':explore') ||
      canonical.endsWith(':explorer') ||
      canonical.endsWith(':code-explorer')
    ) {
      return agent.agentType
    }
  }

  const generalPurpose = agents.find(agent => {
    const canonical = canonicalizeAgentType(agent.agentType)
    return canonical === 'general-purpose' || canonical.endsWith(':general-purpose')
  })

  return generalPurpose?.agentType ?? 'general-purpose'
}

// Render the agent-type section.
//
// When the agent_listing_delta attachment is active (shouldInjectAgentListInMessages()),
// the full list is delivered via <system-reminder> messages — inlining it here
// would duplicate content AND bust the tool-schema prompt cache on every
// agent-list mutation (MCP connect, /reload-plugins, permission change).
// Mirror AgentTool's handling to stay cache-friendly.
function renderAgentTypeSection(agents: readonly PromptAgent[]): string {
  if (shouldInjectAgentListInMessages()) {
    return 'Available agent types are listed in <system-reminder> messages in the conversation. Use `general-purpose` by default.'
  }

  const uniqueAgents = Array.from(
    new Map(agents.map(agent => [agent.agentType, agent])).values(),
  )

  if (uniqueAgents.length === 0) {
    return `Available agent types depend on the current session configuration.
Use \`general-purpose\` by default, and only pass \`subagent_type\` when a specific type is listed in the current agent list.`
  }

  const lines = uniqueAgents.map(agent => {
    const whenToUse = agent.whenToUse.trim() || 'No description provided.'
    return `- \`${agent.agentType}\`: ${whenToUse}`
  })

  return `Available agent types in this session:
${lines.join('\n')}

Use \`general-purpose\` by default. Only set \`subagent_type\` when you need a specific specialized type from the list above.`
}

export function getPrompt(
  options: {
    agents?: readonly PromptAgent[]
  } = {},
): string {
  const agents = options.agents ?? []
  const preferredSearchAgentType = pickSearchAgentType(agents)
  const agentTypeSection = renderAgentTypeSection(agents)

  return `${DESCRIPTION}

## Capabilities

All file ops (Read / Write / Edit / NotebookEdit), web retrieval (WebFetch),
sub-agent orchestration (Agent), and multi-step orchestration route through Script.
Even a single Read / Write / Edit / WebFetch call must go through Script.

Each injected binding is an async function backed by an in-process tool
(same input schema, permission checks, and side effects). Tool and runtime
failures surface as JavaScript exceptions — use try/catch.

${renderBindingTable()}

### Agent binding

\`Agent({ description, prompt, subagent_type? })\` delegates to a sub-agent
with access to the full tool set. \`description\` is 3-5 words;
\`subagent_type\` defaults to \`'general-purpose'\`. If an unavailable
\`subagent_type\` is requested, retry with \`'general-purpose'\`.

${agentTypeSection}

## utils namespace

- \`utils.cwd\` — current working directory (read-only string)
- \`utils.sleep(ms)\` — abort-aware Promise sleep (rejects on abort/timeout)

## Unsupported in Script

- \`Bash\`, \`Glob\`, \`Grep\` are not injected (referencing them throws).
- Static \`import\` / \`export\` are not allowed (code runs inside an async function).

For shell commands, file search, or other sandbox-external work, delegate via
\`Agent({ ... })\` — the sub-agent has the full tool set.

## Execution policy (MUST)

- NEVER output shell / Python / Bash / Node code as text for the user to run.
  Every executable step MUST go through a Script binding or \`Agent({ ... })\`.
- If the task needs tools outside the sandbox (Bash / Glob / Grep, tests,
  package managers, arbitrary shell commands), delegate via \`Agent({ ... })\`.
- If neither channel works (e.g. interactive OAuth / sudo / hardware),
  state the limitation and ask the user — do NOT dump a script for them to run.

## Conventions

- Top-level \`await\` and \`return\` are supported
- \`console.log/info\` -> stdout; \`console.warn/error\` -> stderr
- For existing files, call \`Read({ file_path })\` before \`Write\` or \`Edit\`
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
// Batch edit with loop
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
// Delegate sandbox-external work (Bash/Grep unavailable inside Script)
const result = await Agent({
  description: 'Find TODO comments',
  subagent_type: '${preferredSearchAgentType}',
  prompt: \`Find all lines containing "TODO" in .ts files under \${utils.cwd}/src. Return file paths and line numbers.\`,
})
return result
\`\`\`

## Limits

- Max execution time: ${MAX_TIMEOUT_MS / 1000}s (override via \`timeout_ms\`)
- Max captured output size: ${Math.floor(MAX_OUTPUT_SIZE / 1024)}KB (stdout and stderr each)
`
}
