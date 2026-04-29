# Pare-style benchmark cases for Claude Code

This folder stores deterministic benchmark cases used to compare baseline vs candidate Claude Code behavior on:

- token usage
- accuracy pass rate

## Case file format

Use JSON with this shape:

- `version`: must be `pare-case-v1`
- `cases`: array of cases sorted by `id`

Each case requires:

- `id`: unique case id
- `name`: short name
- `prompt`: one-shot prompt text
- `assertion`: deterministic checker
  - `exact`: exact string match
  - `includes`: substring must be present
  - `regex`: regular expression must match output

Optional:

- `tags`: array of labels

## Run

Build first:

```bash
bun run build
```

Full run with explicit commands:

```bash
bun run benchmark:pare --cases benchmarks/pare/cases/core.json --baseline-cmd bun --baseline-args "dist/cli.js" --candidate-cmd bun --candidate-args "dist/cli.js"
```

Artifacts are written to:

```
.artifacts/benchmarks/pare/<timestamp>/
```

with `baseline.json`, `candidate.json`, and `comparison.json`.

## Migrated Pare v2 case sets

- `benchmarks/pare/cases/pare-v2-reproducible.json`
- `benchmarks/pare/cases/pare-v2-mutating.json`

Run examples:

```bash
bun run benchmark:pare --cases benchmarks/pare/cases/pare-v2-reproducible.json
bun run benchmark:pare --cases benchmarks/pare/cases/pare-v2-mutating.json
```

## Pare alignment fields

Migrated case files now include metadata fields to better match Pare-style reporting:

- `metadata.pareScenarioId`
- `metadata.pareScenarioName`
- `metadata.category`
- `metadata.useFrequency`

The comparison output now includes grouped summaries:

- `grouped.baseline.byCategory`
- `grouped.baseline.byFrequency`
- `grouped.candidate.byCategory`
- `grouped.candidate.byFrequency`


## Workspace isolation

By default benchmark runs use an isolated temporary git clone in the system tmp directory to avoid modifying your current working tree while preserving full git context.

- default: `--workspace-mode tmp-git`
- alternative: `--workspace-mode worktree`
- opt-out: `--workspace-mode current` (not recommended)


## Config file usage

To avoid very long command lines, you can pass a JSON config file via `--config`.

Example config (save anywhere, e.g. `/tmp/pare.config.json`):

```json
{
  "cases": "benchmarks/pare/cases/pare-v2-reproducible.json",
  "runsPerCase": 3,
  "workspaceMode": "tmp-git",
  "permissionMode": "bypassPermissions",
  "baseline": {
    "label": "baseline",
    "command": "env",
    "args": [
      "ENABLE_SCRIPT_TOOL=0",
      "bun",
      "dist/cli.js",
      "--settings=/Users/bytedance/.claude/ttadk.json",
      "--permission-mode",
      "bypassPermissions"
    ]
  },
  "candidate": {
    "label": "candidate",
    "command": "env",
    "args": [
      "ENABLE_SCRIPT_TOOL=1",
      "bun",
      "dist/cli.js",
      "--settings=/Users/bytedance/.claude/ttadk.json",
      "--permission-mode",
      "bypassPermissions"
    ]
  }
}
```

Run with config:

```bash
bun run benchmark:pare --config /tmp/pare.config.json
```

You can still override specific fields from CLI; CLI flags take precedence over config.

Script wrapper also supports pass-through:

```bash
bash scripts/benchmark-script-tool-toggle.sh --config /tmp/pare.config.json
```
