---
name: debug-session
description: |
  Inspect CPP session transcripts and API dumps for debugging tool-call
  rendering, duplicate messages, or model behavior issues. Use when a user
  reports unexpected transcript rendering (duplicate rows, wrong tool
  status, missing output) and you need the raw conversation/API data.
---

# `/debug-session` --- Inspect Session Transcripts & API Dumps

## Overview

The CPP build persists two levels of debug trace per session:

| Layer | Path | Content |
|-------|------|---------|
| **Session Transcript** | `~/.cc-repl/sessions/<session_id>/messages.jsonl` | Every `Message` appended to the engine conversation (complete with `ToolUseBlock`, `ToolResultBlock`, `TextBlock`, `ThinkingBlock`). One JSON object per line. |
| **API Dump (request + response)** | `~/.cc-repl/dump-prompts/<session_id>.jsonl` | Full API request body (messages array, system prompt, tools schema) and parsed response (assembled `AssistantMessage` with all content blocks, stop_reason, has_tool_use flag). One JSON object per line. |

Both are enabled automatically on every session start (no flags needed).

## File Locations

```bash
# List recent sessions
ls -lt ~/.cc-repl/sessions/ | head

# Find the most recent session transcript
ls -lt ~/.cc-repl/sessions/*/messages.jsonl | head -1

# Find the most recent API dump
ls -lt ~/.cc-repl/dump-prompts/*.jsonl | head -1
```

## Reading Session Transcripts

Each line in `messages.jsonl` is a complete `Message` JSON object:

```bash
# Pretty-print the full conversation
cat ~/.cc-repl/sessions/<session_id>/messages.jsonl | python3 -m json.tool --no-ensure-ascii

# Count messages by role
cat messages.jsonl | python3 -c "
import json, sys
from collections import Counter
c = Counter()
for line in sys.stdin:
    msg = json.loads(line)
    c[msg.get('role','')] += 1
print(dict(c))
"

# Find all tool_use blocks (diagnose duplicate tool calls)
cat messages.jsonl | python3 -c "
import json, sys
for i, line in enumerate(sys.stdin):
    msg = json.loads(line)
    if msg.get('role') == 'assistant':
        for block in msg.get('content', []):
            if block.get('type') == 'tool_use':
                print(f'msg[{i}]: tool_use name={block[\"name\"]} id={block.get(\"id\",\"?\")}')" 
```

## Reading API Dumps

Each line in the dump-prompts JSONL is either a `request` or `response`:

```bash
# Show all API rounds (request/response pairs)
cat ~/.cc-repl/dump-prompts/<session_id>.jsonl | python3 -c "
import json, sys
for line in sys.stdin:
    entry = json.loads(line)
    t = entry['type']
    ts = entry.get('timestamp', 0)
    if t == 'request':
        body = entry['body']
        n_msgs = len(body.get('messages', []))
        model = body.get('model', '?')
        print(f'[{ts}] REQUEST: model={model} messages={n_msgs}')
    elif t == 'response':
        sr = entry.get('stop_reason', '?')
        has_tool = entry.get('has_tool_use', False)
        print(f'[{ts}] RESPONSE: stop_reason={sr} has_tool_use={has_tool}')
"

# Extract just the model's response content blocks from round N
cat dump-prompts.jsonl | python3 -c "
import json, sys
responses = [json.loads(l) for l in sys.stdin if '\"response\"' in l]
for i, r in enumerate(responses):
    msg = r.get('message', {})
    print(f'--- Round {i} (stop={r.get(\"stop_reason\")}) ---')
    for block in msg.get('content', []):
        btype = block.get('type', '?')
        if btype == 'text':
            print(f'  TextBlock: {block[\"text\"][:100]}...')
        elif btype == 'tool_use':
            print(f'  ToolUseBlock: name={block[\"name\"]} input={json.dumps(block.get(\"input\",{}))[:80]}')
"
```

## Common Debugging Scenarios

### "Tool-use renders twice in the transcript"

Check how many AssistantMessages contain a ToolUseBlock for the same tool:

```bash
cat messages.jsonl | python3 -c "
import json, sys
for i, line in enumerate(sys.stdin):
    msg = json.loads(line)
    if msg.get('role') == 'assistant':
        blocks = [b for b in msg.get('content',[]) if b.get('type')=='tool_use']
        if blocks:
            print(f'msg[{i}]: {len(blocks)} tool_use block(s): {[b[\"name\"] for b in blocks]}')"
```

If you see TWO assistant messages each with the same tool_use, the model made a
redundant tool call in its second response (after seeing the tool result). This
is model behavior, not a rendering bug.

### "Tool result shows 'Waiting to run' instead of green dot"

Check the `tool_status` field. In the committed conversation, all tool-use
entries should have `tool_status: "success"` (or "error"). If they show
"pending", the `project_messages` function isn't setting the status correctly.

### "Output is missing from tool result"

Check that the ToolResultMessage has non-empty content:

```bash
cat messages.jsonl | python3 -c "
import json, sys
for i, line in enumerate(sys.stdin):
    msg = json.loads(line)
    if msg.get('role') == 'tool':
        content = msg.get('content', '')
        print(f'msg[{i}] tool_result: {str(content)[:200]}')"
```
