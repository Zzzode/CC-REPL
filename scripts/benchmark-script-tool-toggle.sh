#!/usr/bin/env bash
set -euo pipefail

CASES="benchmarks/pare/cases/pare-v2-reproducible.json"
EXTRA_ARGS=()

if [[ $# -gt 0 && "$1" != --* ]]; then
  CASES="$1"
  shift
fi

while [[ $# -gt 0 ]]; do
  EXTRA_ARGS+=("$1")
  shift
done

PERMISSION_MODE="${PERMISSION_MODE:-bypassPermissions}"

bun run benchmark:pare \
  --cases "$CASES" \
  --workspace-mode tmp-git \
  --baseline-cmd env \
  --baseline-args "ENABLE_SCRIPT_TOOL=0 bun dist/cli.js --settings=/Users/bytedance/.claude/ttadk.json --permission-mode $PERMISSION_MODE" \
  --candidate-cmd env \
  --candidate-args "ENABLE_SCRIPT_TOOL=1 bun dist/cli.js --settings=/Users/bytedance/.claude/ttadk.json --permission-mode $PERMISSION_MODE" \
  "${EXTRA_ARGS[@]}"
