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

## Migrated Pare v2 cases

A migrated case file is provided at:

`benchmarks/pare/cases/pare-v2-migrated.json`

It imports scenario names/commands from Pare v2 scenario definitions and adapts them to this harness as deterministic command-fidelity checks.

Run:

```bash
bun run benchmark:pare --cases benchmarks/pare/cases/pare-v2-migrated.json --max-cases 10
```

## Extended migrated case sets

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
