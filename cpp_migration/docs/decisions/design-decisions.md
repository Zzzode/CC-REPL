# Design decisions that survive the TypeScript reference tree

**What this is.** `cpp_migration/src` is a C++23 port of the TypeScript tree at `src/`.
That TS tree is being deleted on **2026-09-21**. The C++ tree carries ~1399 `TS REF: <path>:<line>`
breadcrumbs; the overwhelming majority are pure navigational pointers whose entire content is
"the TS file said this at line N" — worthless the moment `src/` is gone.

This document extracts the minority: comments that record a **decision** or a **non-obvious
constraint** — the things a maintainer cannot re-derive from the C++ code alone, and could not
recover from the TS tree even before it was deleted, because they are about *why* the C++ differs,
*what silently breaks if you change it*, and *which other file you must change in lockstep*.

**How to read an entry.** Every bullet is rewritten to stand alone. Where the TS file name matters
for the meaning ("the TS helper did X"), it is kept in prose; otherwise the pointer is dropped.
Nothing here requires the deleted tree.

**Excluded.** Pure file/line pointers with no stated reason; ordinary validation text
("must not be empty"); and the self-documenting `TS REFERENCE:` header blocks, which are listed in
the appendix instead of transcribed.

**Provenance.** Extracted by grepping `cpp_migration/src` for `TS REF` plus the marker vocabulary
(`deliberately`, `intentional`, `divergence`, `replaces the previous`, `must not`, `cannot`,
`NOTE:`, `CROSS-MODULE`, `contract`, `silent`, `coupling`, `workaround`, `because`, …) and then
reading 10-20 lines of surrounding context for each candidate.

---

**Path convention.** Paths are written relative to the repository root as of
2026-09-21, when the C++ tree still lived under `cpp_migration/`. The tree was
promoted to the repository root in the same series of commits, so
`cpp_migration/src/...` reads as `src/...` from then on.


## A. Intentional divergence — the C++ deliberately differs from TS, and says why

### A.1 — Vim mode / input mode consolidation

- **`cpp_migration/src/vim/vim_types.cppm:8-18`** — The canonical `VimMode` enum deliberately lives
  in a low-level module (`cc_vim`, depending only on `cc_utils`) rather than next to its UI
  consumers. Five mutually incompatible local `VimMode` definitions had grown in the C++ tree
  (a 6-value one in `ui/prompt/vim_input.cppm`, a 3-value one in `ui/prompt_input.cppm`, another
  6-value one in `vim/vim_mode.cppm`, a 5-value one in `hooks/vim_input.cppm`, and a plain
  `bool enable_vim` in `ui/components/text_input.cppm`). They were unified here because
  `cc_hooks` needs `VimMode` while `cc_ui` depends on `cc_hooks`, so any higher placement creates
  a cycle. The resulting enum is `{Normal, Insert, Visual, VisualLine, VisualBlock, Replace,
  Command}`.

- **`cpp_migration/src/hooks/vim_input.cppm:30-36`** — Divergence from the *previous C++ state*:
  this file's local `VimMode` used to be a 5-value enum `{Normal, Insert, Visual, VisualLine,
  Command}` that was **missing `VisualBlock` and `Replace`**. Replaced by the canonical
  `cc::vim::VimMode`. A maintainer adding a new vim mode must add it to `vim_types.cppm`, not to
  any per-consumer enum — the per-consumer enums no longer exist and re-introducing one silently
  resurrects the "missing modes" bug.

- **`cpp_migration/src/vim/vim_mode.cppm:12-17`** — Same consolidation, different casualty: this
  file's local 6-value enum `{Normal, Insert, Visual, VisualLine, Command, Replace}` was missing
  `VisualBlock`. The canonical enum is now re-exported from here for external consumers.

- **`cpp_migration/src/ui/prompt/vim_input.cppm:50-54`** and
  **`cpp_migration/src/ui/components/text_input_widget.cppm:58-63`** — Two more local enums retired.
  `vim_input.cppm` had a 6-value enum missing `VisualBlock`; `text_input_widget.cppm` had a
  5-value enum `{Disabled, Normal, Insert, Visual, Command}` that **conflicted with other
  implementations**. In the widget, "vim disabled" is now expressed as
  `std::optional<VimMode>{nullopt}` rather than a `Disabled` enumerator — code that tests
  `mode == VimMode::Disabled` no longer compiles for a reason.

- **`cpp_migration/src/ui/prompt_input.cppm:25-29`** — This file's local 3-value enum
  `{Normal, Insert, Visual}` conflicted with the 6-value version elsewhere. Retired.

- **`cpp_migration/src/commands/vim.cppm:27-33`** — The `/vim` slash command's local
  `VimModeState` enum `{Normal, Insert, Visual, Command}` also conflicted; "disabled" is tracked
  separately by an `enabled_` bool in this class rather than by an enum value.

- **`cpp_migration/src/ui/prompt_input.cppm:443-447`** — The `VimHandler` class was **removed
  entirely**. Vim handling is now split across three places that must stay coordinated:
  `cc::ui::common::VimMode` (the enum), `cc::ui::prompt::vim_input` (standalone component), and
  `ui::components::TextInputImpl` holding an `optional<VimMode>`. Anyone looking for a `VimHandler`
  type will not find one.

- **`cpp_migration/src/ui/components/text_input_widget.cppm:58-63`** — See above; note additionally
  that the widget keeps its own simplified vim handling: `p`/`P` paste is a **no-op** (no register),
  as is `Ctrl+R` redo (no undo stack). Those are per-widget simplifications, not the full vim
  implementation in `text_input.cppm`.

### A.2 — Prompt input mode as a single overloaded enum

- **`cpp_migration/src/ui/common/ui_types.cppm:81-93`** — `PromptInputMode` is a **unified canonical
  enum** that replaces incompatible definitions scattered across `text_input.cppm`,
  `prompt_input_full.cppm`, `prompt_input_footer.cppm`, `repl_screen.cppm`, and a previous 3-value
  stub in this same file. The TS `PromptInputMode` type is only four values
  (`'bash' | 'prompt' | 'orphaned-permission' | 'task-notification'`), but the C++ port had
  historically mixed **orthogonal** concepts (vim mode, plan mode, history search, prefix-triggered
  modes) into the same enum. The unified definition preserves the full union so existing switches
  compile, but callers are told to treat vim/plan/search as **layered state** — TS parity means
  `VimMode` is a separate type and plan mode is a separate flag, and new code must not add another
  orthogonal concept to this enum.

- **`cpp_migration/src/ui/common/ui_types.cppm:116-119`** — The `inputModes.ts` helpers
  (`prependModeCharacterToInput`, `getModeFromInput`, `getValueFromInput`, `isInputModeCharacter`)
  do **not** live next to the mode enum. They live in `cc::ui::design::figures`, which is declared
  the single source of truth for prompt-prefix glyphs *and* mode-detection utilities. Putting a
  mode-detection helper beside `PromptInputMode` splits that source of truth.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:79-90`** — The footer's mapping onto the
  unified enum is spelled out: `PromptInputMode::Prompt` maps to `PromptInputMode::Normal`
  (TS calls it `'prompt'`); Bash / SlashCommand / HistorySearch / PlanMode are identical in both
  definitions. This file also previously defined a **local 4-value `VimMode`
  `{Normal, Insert, Visual, None}`** — retired, with "None" now `optional<VimMode>{nullopt}`.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:3061-3068`** — Because `InputMode` and the
  footer's `PromptInputMode` are now literally the same unified type, the footer-mode assignment is
  a direct `static_cast` with a bash-detection override (`effective_is_bash()`), not a translation
  table. Text-derived mode takes precedence over the state toggle.

- **`cpp_migration/src/ui/components/text_input.cppm:2352-2361`** — Deliberate divergence in *where*
  the mode-prefix character is stripped. The C++ buffer does **not** strip the leading `!` here.
  Rationale: standalone `TextInput` usage (tests, dialogs) must see the exact text the user typed,
  and history round-trips correctly through `prependModeCharacterToInput` only if the prefix is
  retained. The `!` is stripped **only at value-extraction time** — `figures::strip_mode_prefix`
  in submit paths and the REPL `on_submit` handlers. Stripping it earlier breaks history
  round-tripping.

- **`cpp_migration/src/ui/design_system/figures.cppm:271-275`** — `strip_mode_prefix`'s contract:
  the `!` is a **transient mode trigger**. Once the mode is detected the character is *never*
  stored in the input state (per the TS `PromptInput.tsx` behaviour). The function exists for
  exactly two callers: (a) pasting `"!cmd"` into an empty input (multi-char insertion) and
  (b) rendering history entries that were saved *with* the prefix intact.

- **`cpp_migration/src/ui/design_system/figures.cppm:59-75`** — The prompt-prefix glyph set is
  deliberately reduced to **two rendered variants**: `kPointer` (❯) and `kBashGlyph` (`!`). The old
  C++ `InputMode` enum had extra values `SlashCommand` / `HistorySearch` / `PlanMode` /
  `VimNormal` / `VimVisual`; those are *orthogonal* to the prompt prefix (they are handled by slash
  routing, the Ctrl+R overlay, and the vim status bar) and were removed from the prefix logic. The
  CPP-only indicator glyphs that have no TS counterpart are retained as `kExtra*` symbols purely so
  existing call sites (vim status bar, plan badge) keep compiling — they must **never** be used as
  the prompt prefix, only as badge / status-row content.

- **`cpp_migration/src/ui/common/ui_types.cppm:222-237`** — The same VimMode unification note
  repeated at the `using cc::vim::VimMode` site, with the explicit note that `cc_hooks` needs
  `VimMode` but `cc_ui` depends on `cc_hooks`.

### A.3 — Wiring the client-side SSE / streaming events

- **`cpp_migration/src/ui/messages/collapse_background_bash.cppm:17-24`** — **TAG-FORMAT NOTE
  (TS vs CPP divergence — intentional).** The TS constants module uses **hyphenated** XML tag names
  (`task-notification`, `status`, `summary`). The C++ engine however *emits* **underscored** tags —
  see `local_agent_task.cppm:435`, `local_shell_task.cppm`, and `runtime_registry.cppm:766`, which
  all write `<task_notification>`, `<status>`, `<summary>`. To collapse the messages the C++ tree
  actually produces, this module matches the **C++ wire format (underscore)**. The constants here
  are declared the single source of truth for that decision — so if the emitters are ever changed
  to hyphenated tags, *this* file must change with them, and vice versa.

- **`cpp_migration/src/ui/messages/collapse_background_bash.cppm:44-48`** — The three tag constants
  (`kTaskNotificationTag = "task_notification"`, `kStatusTag = "status"`, `kSummaryTag = "summary"`)
  are the concrete form of the decision above.

- **`cpp_migration/src/services/api/sse_client.cppm:506-509`** — `on_final` is **deliberately not
  fired** on the `message_stop` SSE event. It is fired exactly once by `PostMessagesStream` when
  `curl_easy_perform` returns, so that the HTTP status code is available to the callback. Firing it
  on `message_stop` would report the stream result *before* the transport outcome is known.

- **`cpp_migration/src/services/api/sse_client.cppm:124`** — Dry-run gate: an empty API key means
  the client **never** goes out to the network. This is a hard gate, not a warning.

- **`cpp_migration/src/services/api/sse_client.cppm:26`** — This module deliberately avoids
  importing anything from `cc.services.api.*` so it can be built and tested in isolation.

- **`cpp_migration/src/query/wire_anthropic.cppm:293-300`** — Mapping notes for the Anthropic SSE
  decoder, "all deliberate, to keep the engine's behaviour". `message_start` carries the message
  id/model but `StreamDelta` has no field for them, so only the usage fields travel through.
  `content_block_start` has **no dedicated `StreamDelta` kind**: a text/thinking block start is
  reported as an **empty** `TextDelta`/`ThinkingDelta` (so a consumer that opens a block on its
  first delta still reproduces the engine's possibly-empty block), and a `tool_use` start becomes
  `ToolUseStart` carrying the id and name.

- **`cpp_migration/src/query/wire_anthropic.cppm:306`** — Unknown event names and unparseable
  frames yield **nothing**, "exactly" as the engine does. Do not add a throw or a default-delta
  here.

- **`cpp_migration/src/query/wire_anthropic.cppm:453`** — "Anything else: silently ignored, as in
  the engine."

- **`cpp_migration/src/query/wire_anthropic.cppm:98-100`** — The project builds with
  `-Wmissing-designated-field-initializers`, so callers that want only *some* fields of
  `AnthropicWireOptions` must assign field-by-field; a partial designated initializer is a build
  error.

- **`cpp_migration/src/query/wire_openai.cppm:40-42`** — The OpenAI backend is intentionally
  **STATELESS**: every method derives its output purely from its arguments, so an instance may be
  shared between concurrent requests. This constraint is what forces the streaming limitations
  documented at A.4.

- **`cpp_migration/src/query/wire_openai.cppm:89-91`** — Document blocks degrade to a textual note
  because the OpenAI chat-completions format has no `document` content part (only `text` and
  `image_url`). The base64 payload is **deliberately NOT inlined** — it would be charged as prompt
  tokens with no way for the model to interpret it.

- **`cpp_migration/src/query/wire_openai.cppm:468`** — `top_k` has no OpenAI equivalent and is
  therefore intentionally dropped rather than approximated.

- **`cpp_migration/src/query/wire_openai.cppm:497-501`** — `input.native_computer_tool` is
  **intentionally ignored** (marked `LOSSY`): no native computer-use tool type exists on this wire
  format, so the capability is offered as a plain function whose schema describes the actions
  instead of the vendor-specific `computer_20241022` shape the Anthropic backend emits. Computer
  use still *works*; the wire representation just differs.

- **`cpp_migration/src/query/wire_openai.cppm:530-534`** — Extended thinking has no
  OpenAI-compatible representation. `thinking_enabled` / `thinking_budget_tokens` are
  **deliberately ignored**: sending a `thinking` object would be rejected by most endpoints, and no
  field exists that would make the model emit a `ThinkingBlock` back. Thinking remains an
  Anthropic-backend-only feature.

- **`cpp_migration/src/query/wire_openai.cppm:536-537`** — Endpoints that accept
  `max_completion_tokens` instead of `max_tokens` (newer OpenAI reasoning models) need a
  *different backend or a flag*; `max_tokens` is what the wide set of local servers accept. This is
  a stated, unfixed gap, not an oversight.

- **`cpp_migration/src/query/wire_openai.cppm:24`** — Thinking blocks have no standard field on this
  wire format; the backend documents that explicitly.

- **`cpp_migration/src/query/wire_openai.cppm:173`** — Unknown / vendor-specific finish reasons pass
  through **verbatim** so the engine can inspect them, rather than being normalized away.

- **`cpp_migration/src/query/wire_openai.cppm:549`** — Caller-supplied headers win over defaults,
  **but never duplicate `Content-Type`**.

- **`cpp_migration/src/query/wire_openai.cppm:428`** — `base_url` **must NOT** already include
  `/v1` — the path is appended by `prepare()`. A trailing `/` is tolerated. Passing a base URL with
  `/v1` yields a doubled path segment.

### A.4 — Documented LOSSY / known-limitation divergences in the OpenAI streaming path

- **`cpp_migration/src/query/wire_openai.cppm:668-698`** — **STREAMING CONTRACT.** Because the
  backend is stateless, `StreamDelta` carries no index and no history can be kept:
  - `TextDelta` — appended to the current text block. OpenAI emits text before tool calls, so text
  and tool-call accumulation do not interleave *in practice*.
  - `ToolUseStart` — emitted whenever a delta carries a non-empty `id` or `function.name`. OpenAI
  sends both only in a tool call's **first** chunk, so in practice exactly one start is emitted
  per tool call. If an endpoint re-sends them, the engine sees a repeated start; **consumers must
  treat a start for an already-open call as a no-op**.
  - Argument fragments carry **no id** (it appeared only in the start chunk), so the engine
  **MUST accumulate in arrival order into the most recently started tool call**.
  - `BlockStop` — **never emitted**: the format has no block boundary event. Consumers close open
  blocks on `MessageStop`.
  - `MessageStop` — emitted for a non-null `finish_reason` and **again** for `[DONE]`. The
  `[DONE]` one carries an **empty** `stop_reason` because the backend cannot know whether one was
  already seen; **consumers must treat an empty `stop_reason` as "unchanged"**.
  - **KNOWN LIMITATION:** parallel tool calls streamed with interleaved `index` values cannot be
  demultiplexed without state. Sequential tool calls (what llama.cpp / vLLM / Ollama emit)
  reconstruct correctly; with interleaved parallel calls the fragments of later calls are
  appended to the **wrong** call. A stateful variant of the backend would fix this.

- **`cpp_migration/src/query/wire_openai.cppm:363-369`** — Message ordering is load-bearing: for a
  user turn, the `role:"tool"` messages go **FIRST** so they directly follow the assistant message
  that requested the calls (OpenAI pairs a tool message with the immediately preceding `tool_calls`,
  and several servers reject a tool message with anything in between). The user message carrying the
  turn's remaining parts (text / images / document notes) then follows.

- **`cpp_migration/src/query/wire_openai.cppm:321`** — Tool results never stay inside the containing
  message; each is split out.

- **`cpp_migration/src/query/wire_openai.cppm:719`** — Empty `stop_reason` here is deliberate; see
  the contract note above.

- **`cpp_migration/src/query/wire_protocol.cppm:19-21`** — Design note: the `WireBackend` interface
  is intentionally **narrow and value-oriented** (strings in, engine types out) rather than exposing
  vendor JSON types, so a backend can be implemented and tested without any engine internals.

- **`cpp_migration/src/query/wire_protocol.cppm:108`** — Verbatim MCP input schemas are keyed by
  tool name; when present for a tool, the verbatim schema wins over the simplified one.

### A.5 — Backend/auth provider selection

- **`cpp_migration/src/services/auth/provider_selector.cppm:11-16`** — **Deliberate divergence from
  a TS bug.** Upstream TS has an inconsistency: `client.ts` branches `BEDROCK > FOUNDRY > VERTEX`
  while `providers.ts` says `BEDROCK > VERTEX > FOUNDRY`. The C++ port chose the **`providers.ts`
  ordering** (`BEDROCK > VERTEX > FOUNDRY`) because that is the ordering used by
  `categorizeRetryableAPIError` and other downstream code that dispatches on `getAPIProvider()`.
  Consequence: if both `LOOM_USE_VERTEX` and `LOOM_USE_FOUNDRY` are set, **VERTEX wins**. Do not
  "fix" this to match `client.ts`.

- **`cpp_migration/src/services/auth/provider_selector.cppm:77-78`** — Provider detection is done
  **once at startup**; environment variables are **not re-read mid-session**. Same contract as TS.
  Changing an auth env var in a running process has no effect.

- **`cpp_migration/src/services/auth/gcp_adc.cppm:27-29`** — `google-cloud-cpp` is **intentionally
  not** pulled in. The module implements the exact 3-step OAuth2 flow that
  `google-auth-library` performs internally, to keep the BYOC surface small and auditable. The
  metadata-server path must send `Metadata-Flavor: Google` and uses a short connection timeout so
  it does not hang outside GCP.

- **`cpp_migration/src/services/auth/azure_credential.cppm:409-415`** — `DefaultAzureCredential`
  tries Environment → WorkloadIdentity → AzureCli → ManagedIdentity and returns the **FIRST**
  credential that either succeeds or raises an explicit error; "not configured" (`nullopt`) branches
  are skipped **silently**. The successful source is cached until its token expires, at which point
  the *full* chain runs again. Not thread-safe.

- **`cpp_migration/src/services/auth/azure_credential.cppm:7`** — The Azure SDK for C++
  (`azure-sdk-for-cpp`) is **intentionally NOT** pulled in.

- **`cpp_migration/src/services/auth/sigv4.cppm:459-465`** — The AWS credential chain is a *lite*
  implementation that deliberately does **NOT** attempt STS `AssumeRole`, SSO OIDC, or
  `credential_process`. Those require network calls and/or external binaries, and TS covers them via
  user-facing `awsAuthRefresh` / `awsCredentialExport` shell hooks. The C++ chain covers the 90%
  "it just works on laptop / CI / EC2" case only.

- **`cpp_migration/src/services/auth/sigv4.cppm:400-402`** — `canonical_headers` **must** already
  include `host` and `date` **before** `build_signed_headers_list` runs, or they get added twice.
  The construction order above that line is what guarantees this.

- **`cpp_migration/src/services/auth/sigv4.cppm:570-572`** — IMDSv2 credentials are fetched over a
  direct `httplib` client to `169.254.169.254:80`, guarded by a short timeout (1.5s PUT + 1.5s GET).
  In a restricted environment (firewall / no IMDS) this returns `nullopt`. Callers should cache the
  result until `role_credentials.expiration - 5 min`.

### A.6 — Analytics / telemetry

- **`cpp_migration/src/services/analytics.cppm:4-13`** — This is the local telemetry the decoupling
  plan **chose to KEEP**: one JSON object per line appended under the XDG state directory, with
  **no network path at all**. There is no sink other than the file — no HTTP client is imported
  here, and adding one would be a *deliberate, separate change* rather than a configuration flag.
  The previous module of this name was a dead stub with zero importers; this is a real writer and is
  wired into the engine so it does not become dead too.

- **`cpp_migration/src/services/analytics.cppm:15-20`** — **Why append-only and why resilient.**
  Telemetry is diagnostic, never load-bearing. If the file cannot be opened the event is dropped and
  logging does not fail: an unwritable state directory **must not break a session**. Flush happens
  **per event** rather than on a buffer threshold, because a crash is exactly the case where the log
  is worth having.

- **`cpp_migration/src/services/analytics.cppm:80`** — `Record one event` never throws and never
  fails a session.

### A.7 — Rendering: markdown

- **`cpp_migration/src/ui/markdown.cppm:1080-1082`** — The heading renderer previously diverged: the
  **prior divergent renderer added cyan/white coloring, which TS does not**. TS applies only
  `chalk.bold.italic.underline` attributes with no color. Do not re-add heading colors.

- **`cpp_migration/src/ui/markdown.cppm:1101-1110`** — The code-block renderer previously called
  `RenderCodeHighlight`, which injected a copy corner tag, a status bar (`[j/k] scroll [q] close`),
  and line-number gutters — **none of which appear in TS output**. That chrome polluted every fenced
  code block in assistant messages. The faithful shape is: the highlighted text **only**, no border,
  no line numbers, no status bar, no `[Copy]` tag. The current implementation uses the existing
  tokenizer with `show_line_numbers=false` and no surrounding frame.

- **`cpp_migration/src/ui/markdown.cppm:1248-1252`** — The **prior divergent renderer used a cyan `•`
  bullet and a 2-space indent — neither appears in TS**. TS emits `${'  '.repeat(listDepth)}- `:
  a plain `- ` with `'  '`-per-depth indentation. Do not re-introduce a bullet glyph.

- **`cpp_migration/src/ui/markdown.cppm:1339-1343`** — Blockquotes: the **prior divergent renderer
  used a blue bar, a non-dim content, and a background color — all absent in TS**. TS uses a DIM `▎`
  (U+258E) prefix with the content italic at *normal* brightness (the `dim` applies to the bar only,
  not the text; the TS comment notes dim text is nearly invisible on dark themes).

- **`cpp_migration/src/ui/markdown.cppm:1376-1380`** — Tables: the **previous divergent renderer
  emitted a plaintext ASCII pipe table (`|---|---|`) which leaked literal `---` dashes into the
  rendered text** (a P0 bug: the GFM separator line leaked as paragraph content). The fix uses
  box-drawing `─` characters instead of ASCII hyphens, which both prevents any spurious `---`
  substring match and matches TS's terminal-native output.

- **`cpp_migration/src/ui/markdown.cppm:1505-1508`** — Horizontal rules: the **prior divergent
  renderer used `ftxui::separator()`, drawing a full-width `──` rule — absent in TS**. TS's
  `formatToken 'hr'` returns the literal 3-character string `"---"`.

- **`cpp_migration/src/ui/markdown.cppm:976-977`** — Strikethrough (`del`) is **intentionally
  disabled** because the TS `configureMarked` disables the `del` tokenizer. Enabling `~~` parsing
  here would be a fidelity regression, not a feature.

- **`cpp_migration/src/ui/markdown.cppm:196`** and **`markdown.cppm:8`** — Same decision restated at
  the parser level and in the file header.

- **`cpp_migration/src/ui/markdown.cppm:784-791`** — The GitHub issue-reference linkifier uses
  **manual scanning instead of `std::regex`** for performance, because markdown rendering is on the
  hot path for streaming updates. The regex equivalent is documented in the comment; the behaviour
  must match it exactly.

- **`cpp_migration/src/ui/markdown.cppm:230-259`** — CommonMark boundary rules implemented by hand:
  an opening emphasis marker must not be preceded by an alphanumeric (`foo*bar` is not emphasis);
  the closing marker must not be followed by whitespace; an opening `$` must not be followed by
  whitespace or another `$` (so `$$...$$` block math is not matched inline), and a closing `$` must
  not be preceded by whitespace.

### A.8 — Rendering: messages pipeline & row shapes

- **`cpp_migration/src/ui/messages/messages_list.cppm:2351-2367`** — The **divergent**
  `RenderMessageRowByType` / `render_message_envelope` path (avatar column + role pill + top accent
  border) is **NOT faithful to TS** — "it added invented chrome". Core message types
  (user, assistant text, thinking, system text, tool-use) are therefore **BYPASSED** around it and
  emit the faithful `Element` directly. Sub-types not yet ported still flow through the divergent
  path. New row types should be added to the faithful path, not the divergent one.

- **`cpp_migration/src/ui/messages/tool_use_message.cppm:636-640`** — `RenderToolUseMessage` /
  `ToolUseMessage` are explicitly the **divergent FTXUI reimplementation** (borders, status pills,
  footers, parameter blocks — "all invented chrome not in TS"). The faithful
  `RenderFaithfulToolUseMessage` is the TS-parity path.

- **`cpp_migration/src/ui/messages/assistant_text_message.cppm:478-479`** — "No header label, no
  timestamp, no separator, no action buttons, no token footer — **the existing divergent Component
  adds all of those**."

- **`cpp_migration/src/ui/messages/thinking_message.cppm:567-569`** — The faithful thinking render
  has no header decoration, no spinner, no token count, no border, no budget bar, no toggle hints —
  all of those belong to the **richer divergent panel** which is kept only for the interactive UI.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2860-2872`** — **This replaces the previous
  "turn-boundary" model**, which incorrectly suppressed `marginTop` on **ALL** assistant blocks after
  a user row, causing (a) zero gap between user and assistant text (too small) and (b) inconsistent
  spacing depending on whether tool results were present. The correct model: `addMargin=true` for
  every message, with two TS-matching exceptions — `UserToolResultMessage` receives no `addMargin`
  (tool results sit flush against the preceding tool_use), and user continuations
  (`isUserContinuation` in TS) suppress `marginTop` so the `⎿` connector renders correctly.

- **`cpp_migration/src/ui/messages/messages_list.cppm:1270-1272`** — Hidden thinking rows are
  **skipped entirely** rather than rendered as an empty element: TS returns `null` (zero height) but
  **FTXUI `vbox` always allocates 1 line per child**, so emitting them would add a phantom blank
  line. They stay visible if selected (user-expanded) or if they are the streaming tail.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2979-2986`** — Leading elements (the logo
  card) are **intentionally NOT included** in the pin-to-bottom height estimate. Including logo
  height would cause premature pin-to-bottom with just 1-2 messages. Correct behaviour: when
  messages alone fit, the user sees logo + all messages top-aligned; when messages overflow,
  pin-to-bottom engages and the logo scrolls off-screen.

- **`cpp_migration/src/ui/messages/messages_list.cppm:3602-3604`** — `streaming_tail_row` is
  **intentionally NOT hashed** into the render-invalidation key: it changes every frame during
  streaming and the `Render()` body already re-reads it fresh on each paint, so no cache
  invalidation is needed. Adding it to the hash would defeat the cache.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2487-2493`** — `is_selected` (row navigation
  highlight) does **NOT** mean "expanded" in the TS sense. Expansion requires an explicit user
  gesture (Ctrl+O / Enter) through the interactive Component path. On the plain-`Element` render
  path, `selected` merely lifts the "hide on complete" guard so the collapsed label is visible;
  full thinking content is driven by the user's Ctrl+O transcript toggle.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2523-2527`** — The bridge between the
  **divergent model** (`ToolResultOptions`, carrying both an `output` field and a separate
  `error_message`) and the **faithful model** (`ToolResultFaithfulData`, a single `content` field
  plus a `kind` enum that drives dispatch). Both models coexist; new tool-result work should target
  the faithful one.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2363-2367`** — See the live-path faithful
  render note above; the divergent envelope path is explicitly *not* the TS shape.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2690-2697`** — The **DIVERGENT PATH**
  (sub-types not yet ported to faithful) is where the unported message kinds are handled. Callbacks
  such as `on_retry` and `on_clear_session` are threaded there.

- **`cpp_migration/src/ui/messages/message_image.cppm:222-226`** — **Deliberately NO "Alt:" line** in
  the image renderer. The `alt_text` field was being abused to stuff base64 PNG data
  (`repl_screen.cppm:825-831`) for a planned "ASCII-art thumbnail" feature that was never
  implemented. Showing a raw base64 prefix (`Alt: iVBORw0KGgo...`) wastes a line and confuses users.
  **Removed 2026-07-04 per a spacing bug report.**

- **`cpp_migration/src/ui/screens/repl_screen.cppm:1226-1229`** — Same decision at the projection
  site: deliberately do **NOT** stuff `ib.data` into `alt_text`.

- **`cpp_migration/src/ui/messages/user_text_message.cppm:424-433`** — `UserCommandMessage` **ALWAYS**
  renders `backgroundColor=userMessageBackground`; there is **no bgcolor swap on selection**.
  Selection affects `BLACK_CIRCLE` color in assistant rows and the `isSelected` context, **not** this
  chip's tint — unlike `UserPromptMessage`, which *does* swap to `messageActionsBackground`. Hence
  `bg = kUserBg` regardless of `is_selected`. The `is_selected` parameter is accepted and ignored.

- **`cpp_migration/src/ui/messages/message_tool_result.cppm:441-445`** — Only tools whose results are
  **LLM-generated natural language** get markdown rendering (AgentTool, BriefTool, ExitPlanModeTool
  use `<Markdown>`). **MCP tools (like `analyze_image`) do NOT** — they route to `MCPTextOutput` →
  `OutputLine` (plain ANSI text only).

- **`cpp_migration/src/ui/messages/message_tool_result.cppm:290-297`** — The MCP JSON-result
  unwrapper only processes content that **STARTS** with `{` (a JSON object), allowing at most 4
  top-level keys and requiring one "dominant" string value (>200 chars, or containing `\n` and >50
  chars). Content such as `analyze_image_result_summary: [{"text":"..."}]` does **not** start with
  `{`, so it returns `nullopt` and the raw content is displayed — this matches TS screenshot
  behaviour.

- **`cpp_migration/src/ui/messages/message_tool_result.cppm:849-852`** — `add_margin` defaults to
  **true** because TS always gives tool results `marginTop=1` (they never have metadata); the caller
  threads the turn-boundary-computed value.

- **`cpp_migration/src/ui/messages/local_command_output_message.cppm:351-352`** — The diamond
  separator is rendered in the **background color** to make it invisible. FTXUI has no
  `Color::Background`, so the C++ version makes the diamond intentionally invisible by simply
  omitting it — do not "restore" it as visible chrome.

- **`cpp_migration/src/ui/messages/message_pipeline.cppm:633-639`** — The visible-index stage
  **intentionally does not use `should_hide_row`**, to keep the filter function signature cheap (no
  payload copy into a `HideContext`; the caller already has the visibility boolean). It is exposed
  as a pure function so the test suite can exercise gap-edge cases without a full render
  environment.

- **`cpp_migration/src/ui/messages/message_pipeline.cppm:575-579`** — The visibility rules,
  restated as a block: redacted thinking blocks → **always hidden** (TS never shows them); completed
  thinking → hidden unless the user is interacting; compacted turns → hidden; **silent bridge tool
  executions** (zero-char preview + no error) → hidden, because these are auto-spawned setup tools
  the user never asked to see.

- **`cpp_migration/src/ui/messages/message_pipeline.cppm:207-214`** — **PRINCIPLE.** The TS
  `messagesSlice.extractTags` runs **BEFORE** a user utterance is appended to conversation state, so
  the transcript never sees the raw XML wrappers. The `<bash-input>` tag specifically is added by
  shell input mode (not typed by the user) and **must be peeled off before display**.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:1131-1134`** — We **no longer early-return on
  empty entries**. The `leading_element` (welcome/logo card) **must always be rendered inside the
  `yframe`** so it scrolls with messages. `messages_list` handles empty rows gracefully via its own
  `visible.empty()` path. An early return here would strand the logo outside the scroll region.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:3216-3219`** — TS upstream does **NOT** render a
  brand pill in the footer (`PromptInputFooter.tsx` has zero occurrences of "LOOM"/"Loom" text).
  Branding is rendered by `CondensedLogo` **only** in the top header. Do not add footer branding.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:3008-3010`** — The `LeftSide` footer row is
  **always exactly 1 row** so scroll content never shifts when hints change. `StatusLine` adds a row
  when present, but is conditionally shown only in prompt mode + not short.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2249-2258`** — The native terminal cursor is
  **intentionally left parked at the screen bottom-right (Hidden)** rather than being declared at
  the prompt caret. Reason: FTXUI's `ScreenInteractive` emits a cursor-**MOVE** sequence every frame
  (from bottom-right to the declared position) even when nothing else changed, and many terminals
  render those hidden-cursor moves as **visible flicker** during the ~20Hz idle re-render. Leaving
  the cursor at bottom-right makes the move delta zero, so FTXUI emits no move and the idle frame is
  flicker-free. The visible caret is still drawn by `TextInputImpl` as an inverted glyph.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:4207`** — Any character is **silently consumed**
  to prevent prompt-injection in that handler.

### A.9 — Rendering: figures, tokens, logo, layout

- **`cpp_migration/src/ui/design_system/figures.cppm:5-9`** — Every user-visible glyph is defined
  **ONCE** here, because render sites scattered across 10+ `.cppm` files had each hard-coded
  slightly different UTF-8 byte sequences or outright wrong characters — the source of the C++
  "prefix glyph three fights" bug across rounds 1-6. Do not inline a glyph literal at a render site.

- **`cpp_migration/src/ui/design_system/figures.cppm:187-193`** — **The previous C++ value was
  wrong**: `kBridgeReadyIndicator` used the emoji `✅︎` (U+2705 + VS15) when TS uses `·✔︎·`
  (middot U+00B7 + heavy check U+2714 + VS15 U+FE0E + middot). Corrected to match TS exactly.
  `kBridgeReadyIndicatorLegacy` is kept for one release so callers that already imported the old
  symbol do not break — and is marked deprecated.

- **`cpp_migration/src/ui/design_system/figures.cppm:44-51`** — Two symbols exist for the prompt
  pointer on purpose: `kPointer` is the glyph alone **and ** `kPointerPrefix` is glyph + ASCII space
  (2 display cells, matching TS's glyph+NBSP pair). Use `kPointerPrefix` **inside `hbox` layout** so
  the trailing cell does not get squashed by `flex`.

- **`cpp_migration/src/ui/design_system/figures.cppm:16-18`** — **LANGUAGE NOTE:** all constants are
  `inline constexpr std::string_view` with **explicit UTF-8 byte escapes** so every compiler and
  platform sees the same bytes. Writing a raw non-ASCII literal here would reintroduce the
  platform-dependent glyph bug.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:314-317`** — The TS dark `background`
  token is `rgb(0,204,204)`, which is a **KNOWN non-rendered TS artifact** (Ink never paints it; the
  audit flags it as a likely false value). The C++ port **keeps the real app-chrome dark
  `rgb(32,33,36)`** rather than paint the UI bright cyan — "faithful-in-spirit, avoids a destructive
  visual change". Do not "correct" this to match the TS literal.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:344-347`** — The dark diff tokens were
  **corrected from GitHub-bright greens/reds to TS's saturated dark shades**. Pinned values:
  `diffAdded=rgb(34,92,43)`, `diffRemoved=rgb(122,41,54)`, `diffAddedWord=rgb(56,166,96)`,
  `diffRemovedWord=rgb(179,89,107)`.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:306-307`** — `accent` was **corrected**
  from a sky-blue `rgb(110,151,255)` to the TS lavender `rgb(177,185,249)` (identical to
  `permission`/`suggestion`) so dialog/info accents match TS.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:561-563`** — Daltonized theme variants
  use **explicit `rgb()` literals ported verbatim from TS**, **NOT** a matrix approximation. A future
  maintainer tempted to "generate" the daltonized palettes would produce different colors.

- **`cpp_migration/src/ui/layout/yoga.cppm:11-36`** — The C++ flexbox is **intentionally a SMALL
  single-pass** implementation covering only what the C++ UI actually consumes (row/column direction,
  flex-grow distribution, five justify-content values, four align-items values, fixed integer
  width/height, uniform margin/padding). **Deliberately NOT ported**: flex-wrap / align-content,
  flex-shrink / flex-basis, min/max constraints and multi-pass clamping, measure functions, and
  absolute/relative positioning. The reason given is that **no caller in `cpp_migration/src` builds
  a `LayoutNode` tree or calls `compute_layout`** — the UI renders through FTXUI elements. Porting
  more of Yoga is only justified when a caller exists.

- **`cpp_migration/src/ui/layout/logo_v2.cppm:358-367`** — The notice aggregator's **order matters**
  and matches TS exactly: Voice → Opus → Channels → Debug → Emergency → Tmux → OrgAnnounce →
  Sandbox → StatusNotices → Guest/Overage. Additionally, GuestPasses/Overage appear inside
  `CondensedLogo`'s right column in TS *and* in `FeedColumn`; the C++ renders them **last** in the
  aggregated stack so both paths can consume one helper, and `CondensedLogo` callers may hide them
  by setting bools off.

- **`cpp_migration/src/ui/layout/logo_v2.cppm:1264-1269`** — `format_welcome_message` produces
  "Welcome back!" for an empty/absent username or one longer than 20 chars, else
  "Welcome back {username}!". **The first-run "Welcome to Loom" variant is NOT produced by this
  function** — it is handled separately by the `WelcomeV2` card renderer.

- **`cpp_migration/src/ui/layout/logo_v2.cppm:1303-1306`** — Ink's `borderText` embeds text directly
  into the border stroke; FTXUI's closest equivalent is `ftxui::window(title, body)`, which paints
  the title inside a `╭─{title}─…─╮` box. The visual structure (inset title on the top border,
  rounded-style border) is preserved — this is an approximation, not an exact port.

- **`cpp_migration/src/ui/layout/logo_v2.cppm:834-836`** — Apple Terminal **light** has only **2**
  clawd rows (not 3): TS shows t16 (top) + t17 (bar), with no separate t18 clawd row before the
  footer. Getting this wrong by symmetry with the other themes is the obvious mistake.

- **`cpp_migration/src/ui/layout/logo_v2.cppm:1512-1513`** — The cwd line's width budget is
  `agentName ? MAX(50) - 1 - name.size() - 3 : 50`, where the fixed prefix cost is the agent name
  plus `"@"` plus `" · "` (1 + name + 3).

- **`cpp_migration/src/ui/layout/logo.cppm:247-249`** — TS appends `" v" + version` **literally**
  after "Loom", with a single space separator: **no line break, no trailing tag like "-cpp"**. Adding
  a suffix here is a fidelity break.

- **`cpp_migration/src/ui/layout/logo.cppm:226-227`** — The Loom mascot is 9 columns per row (the
  "std" renderer, not the apple-terminal narrow 7-column fallback), and the box width may be off by
  ±1 on terminals with ambiguous emoji/block width — accepted.

- **`cpp_migration/src/ui/components/shell_progress_message.cppm:105-108`** — The MB formatter used
  2 decimal places while **TS uses `toFixed(1)` for GB too**; the faithful port switched GB to 1
  decimal. The trailing `.0` is stripped in both cases.

- **`cpp_migration/src/ui/components/partial_completions.cppm:448-450`** — CamelCase identifiers are
  highlighted with `MagentaLight` to distinguish them from keywords **without conflicting** with the
  `code_highlight.cppm` `type_name = magenta/cyan` convention. Changing this color breaks
  cross-module visual alignment.

- **`cpp_migration/src/ui/components/file_edit_tool_diff.cppm:193-195`** — The structured-diff hunk
  `header` string is **intentionally left empty**: TS's `StructuredDiff` generates the `@@` line from
  start/line counts, and the C++ does the same inside `RenderHunkHeader` from
  `old_start`/`old_lines`/etc. Populating `header` here would produce a duplicated `@@` line.

- **`cpp_migration/src/ui/components/structured_diff.cppm:116-123`** — A full AST-based structural
  diff is **out of scope** (TODO: wire up the LSP-based syntax tree when available). Until then a
  lightweight heuristic mirrors the strategy in `StructuredDiff/Fallback.tsx`:
  empty-line-separated logical segments, indentation-based depth boundaries, and
  comment/import/class/function sentinels.

- **`cpp_migration/src/ui/components/structured_diff.cppm:306-315`** — Word-level diff uses a
  lightweight **O(N*M) LCS over UTF-8 code units** rather than the reusable Myers implementation,
  because the Myers algorithm in `utils/file_edit` is **line-level and not exported at character
  granularity**. Inputs are bounded to single-line lengths so the quadratic cost is acceptable.
  `Deferred(#ui7-word-diff)`: export a char-level Myers from utils to consolidate.

- **`cpp_migration/src/ui/components/code_highlight.cppm:278-283`** — A full LSP-backed syntax
  highlighter is **out of scope** (Deferred: integrate with `cc/utils/cli_highlight` / the HL module
  once exposed). The heuristic keyword + string + number + comment tokenizer mirrors
  `HighlightedCode/Fallback.tsx` and deliberately covers 8 common languages without pulling in
  grammars' worth of code.

- **`cpp_migration/src/ui/components/pr_badge.cppm:67`** — FTXUI has no `Link` component, so the
  badge is displayed without a link affordance.

- **`cpp_migration/src/ui/components/fast_icon.cppm:37`** — Color application would depend on the
  theme system, which this component does not reach into.

- **`cpp_migration/src/ui/messages/thinking_message.cppm:39`** — Threshold (seconds) after which
  active thinking shows a "still thinking" banner; if duration is zero, the static `duration` field
  is used instead.

- **`cpp_migration/src/ui/renderer/text_measure.cppm:135-138`** — `is_wide_char` follows UAX #11
  East Asian Fullwidth/Wide plus some ambiguous ranges commonly treated as wide in CJK terminal
  contexts, plus emoji. This is a deliberate approximation of `wcwidth`, not a conformance
  implementation.

### A.10 — UI interaction / input handling

- **`cpp_migration/src/ui/dialogs/mcp_dialogs.cppm:1249-1251`** — **Enter no longer toggles
  booleans** — that was a **UX bug**. Space toggles bools; Enter always accepts the form when not
  otherwise handled by a popup or button. Reverting this silently reintroduces the bug.

- **`cpp_migration/src/ui/screens/log_selector.cppm:366-372`** — The UI19-style session card here is
  a **verbatim copy** of `resume_screen.cppm`'s `RenderSessionCard` shape (same icon/colour tokens,
  same split layout, same selected/hovered treatment). It is **inlined rather than imported** so
  `log_selector` stays a self-contained P0 module. The comment is retained specifically so the style
  stays in sync with UI19 by hand — changing one without the other is the anticipated mistake.

- **`cpp_migration/src/ui/dialogs/cost_threshold_dialog.cppm:105-107`** — The title uses
  `std::lround` so that e.g. `$4.70` displays as `"$5"`, matching the **exact `sprintf("%.0f")`
  semantics required by the contract**. `std::to_string` on the rounded long matches `sprintf("%lld")`
  for the always-non-negative spend value.

- **`cpp_migration/src/ui/dialogs/cost_threshold_dialog.cppm:78-82`** — The TS counterpart uses a
  `<Select>` with a single option; because there is only one option the C++ keeps `selected_index`
  **fixed at 0** and allows Arrow keys to be **no-ops** (the index never leaves `[0, 0]`).

- **`cpp_migration/src/ui/dialogs/cost_threshold_dialog.cppm:219`** — Arrow keys and Tab are
  **swallowed** so they never reach the parent component.

- **`cpp_migration/src/ui/dialogs/dialog_system.cppm:397-401`** — **P0x3 CONTRACT — DO NOT ADD
  fabricated Continue/Reset/Quit actions** to the CostThreshold dialog. The contract is:
  `dollars_spent` formatted into the title with `$%.0f`; optional `model_name` rendered for context;
  `on_done()` is a 0-arg `void()` callback; **both Enter AND Escape invoke `on_done()`** with no
  data-loss exits.

- **`cpp_migration/src/ui/dialogs/cost_threshold_dialog.cppm:9`**, **`:67`**, **`:73`**, **`:192`** —
  The same contract restated: exactly one terminal callback per prompt; Enter (commit selection) and
  Escape (`Dialog.onCancel`) both dismiss; no fabricated actions.

- **`cpp_migration/src/ui/dialogs/dialog_system.cppm:304-311`** — Only `MessageSelector` (band 1)
  shows **while the user is typing**; bottom-slot banners 2..6 are all hidden during typing so focus
  remains on the user's input. Overlay and Modal/Standalone have their own typing rules in
  `should_show_dialog()`. Implemented as `priority_for(type) != Band1`.

- **`cpp_migration/src/ui/dialogs/dialog_system.cppm:247`** — Band 0 (highest) holds **implicit exit
  states**.

- **`cpp_migration/src/ui/dialogs/dialog_system.cppm:1365`** — Band 1 (index 0) is `MessageSelector`
  and is **never suppressed**.

- **`cpp_migration/src/ui/dialogs/triggers.cppm:387-389`** — `PushTrustDialog` **intentionally leaves
  `p.state` null**: the renderer builds the `trust_dialog::DialogState` from the `id` on first
  render. Populating it eagerly duplicates state ownership.

- **`cpp_migration/src/ui/dialogs/triggers.cppm:488-496`** — For `"UI:model-picker"` the payload's
  `target` is left **empty** so the renderer shows the **full model list** instead of a single
  confirmation banner, and `on_response` is a **no-op** because the actual switch is performed by the
  command handler dispatching `SwitchModel` through the AppState action system.

- **`cpp_migration/src/ui/dialogs/plugin_dialog.cppm:770-775`** — TS has **no 5-card dashboard** — it
  goes straight to tab navigation. Legacy menu view-kinds (`ViewKind::Menu`,
  `ViewKind::MarketplaceMenu`) are therefore **normalized to the Discover tab** rather than rendered
  as cards.

- **`cpp_migration/src/ui/dialogs/dialog_default_renderers.cppm:635-646`** — Six dialog types
  (`ManagedSettingsSecurity`, `FeedbackSurvey`, `GlobalSearch`, `HistorySearch`, `PluginDialog`,
  `DiffDialog`) are **intentionally NOT registered**. No trigger paths exist yet for these types, and
  **registering a stub renderer would be dead code / a misleading placeholder**. When the chrome is
  actually ported, add the real FTXUI components and call `registry.register_dialog(...)` one block
  per pair.

- **`cpp_migration/src/ui/dialogs/dialog_launchers.cppm:262-270`** — `AppStateBundle` is
  **deliberately small**: setup dialogs **MUST NOT** access the full AppState, because doing so pulls
  huge module dependencies into the launcher path.

- **`cpp_migration/src/ui/dialogs/wizard_dialog.cppm:79`** — The wizard step's render callback
  **MUST NOT** be null; it *is* the content.

- **`cpp_migration/src/ui/dialogs/trust_dialog.cppm:777-778`** — **DO NOT re-implement the trust
  model**: this short-circuits through the existing `cc::commands::get_trust_level`.

- **`cpp_migration/src/ui/dialogs/trust_utils.cppm:273`** — Malformed patterns are skipped — the scan
  **never fails**. A throwing regex here would take down the trust UI.

- **`cpp_migration/src/ui/dialogs/trust_utils.cppm:116-130`** — These regexes match **path names**
  (filesystem paths or URLs), not arbitrary text. A `std::array` of C-strings compiled at call-site
  keeps this cheap.

- **`cpp_migration/src/ui/dialogs/trust_utils.cppm:319`** — `trust_utils` is **the single source of
  truth** the `TrustDialog` uses to pick its UI tier. Introducing a second tier decision elsewhere
  splits the model.

- **`cpp_migration/src/ui/dialogs/about_dialog.cppm:77-80`** — The About dialog's `website` field is
  **empty unless the user configures `LOOM_DOCS_BASE`**: no docs site ships with this project, and
  the vendor's URLs **must not** be re-pointed at a host that does not resolve.


- **`cpp_migration/src/bridge/config.cppm:241-246`** — `getBridgeBaseUrl()` returns **empty** rather
  than any vendor host: no hosted bridge service ships with this build, and **the upstream vendor's
  host must not be renamed into one that does not resolve**. Empty signals "not configured" to
  callers.


- **`cpp_migration/src/ui/prompt/placeholder_cascade.cppm:173-178`** — TS also checks
  `!proactiveModule?.isProactiveActive()`; the C++ **omits** the proactive-mode gate because the
  engine controls this via `prompt_suggestion_enabled` (set false in proactive mode), which is
  equivalent in effect. If that coupling is ever broken, this placeholder becomes wrong.

- **`cpp_migration/src/ui/prompt/placeholder_cascade.cppm:52-55`** — The TS version samples example
  commands from git history; the C++ uses a **static list sufficient for the placeholder UX** without
  requiring git access at render time.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:513-527`** — The `BuiltinStatusLine` has
  **no TS equivalent** — it is a CPP-only enhancement for standalone usability, showing folder, git
  branch, model, context usage, and cost. It kicks in when the user's `statusLine.command` returns
  empty output; **the user's configured command output takes priority when available**.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:1110-1112`** — The `ProRenewal` pill has
  **no direct TS equivalent** (CPP enhancement); it shows a renewal reminder only when
  `days_remaining < 7`.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:520`** — See above; the CPP-only
  enhancement is explicitly flagged so it is not mistaken for a fidelity bug.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:1152-1164`** — Notification priority chain,
  highest first: the **voice indicator replaces every other notification** while the session is
  recording or processing. Crucially, **idle falls through: TS idle renders `null` and must not add
  a row**. `voice_enabled` mirrors TS `voiceEnabled` — the indicator is suppressed entirely unless
  voice is enabled.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:59`** — P0-1: the active theme provider is
  used for **bash-border consistency** (BUG-3 fix) so the prompt prefix and the footer's bash chrome
  agree.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:197-198`** — The bash-mode banner uses the
  `bash_border` token for consistency with the prompt prefix **and** the transcript
  `user-bash-input` bubble — this was a **3-sites bash-border divergence** (BUG-3), now fixed. All
  three sites must keep using the same token.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:388-391`** — The footer shows
  `"-- INSERT --"` **only in insert mode**; normal/visual modes show nothing extra in the footer
  (the mode indicator covers them).

- **`cpp_migration/src/ui/prompt/mode_indicator.cppm:13-16`** — **CPP PRIORITY NOTE:** when a viewing
  agent is active, the prompt prefix is **ALWAYS** ❯ (never `!`), tinted with that agent's color —
  matching TS, where `viewingAgentName ?` is checked **BEFORE** `mode === 'bash'`. Getting this
  precedence backwards is the obvious regression.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2153-2163`** — The same precedence restated at the
  REPL site, with the exact ordering (`viewingAgentName` → `bash mode` → default).

- **`cpp_migration/src/ui/prompt/mode_indicator.cppm:121-124`** — The default branch **deliberately
  emits no ANSI escape** rather than `"\033[37m"` (white): white overpowers the terminal theme in
  light-mode / daltonized themes. TS uses `color={undefined}`, i.e. Ink's default text color.

- **`cpp_migration/src/ui/prompt/mode_indicator.cppm:24-29`** — The TS "viewing agent" concept maps to
  `active_agent` in `PromptInputFullProps`; the fallback path (no active agent but a teammate color
  supplied) corresponds to TS's `getTeammateThemeColor()` + `isAgentSwarmsEnabled()`.

- **`cpp_migration/src/ui/prompt/fuzzy_rank_nucleo.cppm:5-16`** — A hard **contract**: `fuzzy_rank_ascii`
  in `app.cppm` buckets candidates into 4 coarse grades `{0=exact, 1=prefix, 2=substring,
  3=subsequence}`, and the existing tier-offset model **relies on this NARROW 0..3 base range** —
  alias adds +1, skill +4, plugin +6. The four buckets are what keep categories separated under
  rank-ascending sort. This module upgrades match *quality* (boundary/camel/consecutive/gap/path
  bonuses) while **keeping the identical {0..3} contract**; widening the range silently breaks
  category separation.

- **`cpp_migration/src/ui/prompt/fuzzy_rank_nucleo.cppm:131-139`** — The contract preserved verbatim:
  empty query → 1000 (everything ties; alphabetical order dominates); case-insensitive exact → 0;
  result always in `{0,1,2,3}`. The hidden-command escape hatch (rank = **-1000**) is set directly at
  its call-site, not here.

- **`cpp_migration/src/ui/prompt/fuzzy_rank_nucleo.cppm:31-34`** — The scoring constants
  (`SCORE_MATCH = 16`, `BONUS_BOUNDARY = 8`, `BONUS_CAMEL = 6`, `BONUS_CONSECUTIVE = 4`,
  `GAP_START = -3`, …) are **mirrored from `cc.utils.file_index` so the two stay in sync without a
  hard import**. Changing one without the other makes the two scorers disagree.

- **`cpp_migration/src/ui/prompt/combined_highlights.cppm:218-222`** — `/btw` detection is anchored
  to the **start** of the text with a word boundary and case-insensitive (TS
  `BTW_PATTERN = /^\/btw\b/gi`). Dropping the `^` anchor turns mid-sentence "/btw" into a trigger.

- **`cpp_migration/src/ui/prompt/placeholder_cascade.cppm:45-50`** — Two pinned limits:
  `kMaxTeammateNameLength = 20` (maximum teammate/agent display name length before truncation) and
  `kQueueHintMaxShowCount = 3` (number of times the queue hint can be shown before being suppressed).

- **`cpp_migration/src/ui/components/text_input.cppm:205`** — When the TabReverse callback is set,
  `Shift+Tab` is **consumed by that callback** instead of navigating history.

- **`cpp_migration/src/ui/components/text_input.cppm:553`** — Best-effort: if no callback is set, the
  operation does **NOT** block; callers are told to check.

- **`cpp_migration/src/ui/components/text_input.cppm:715`** — The text input does **not consume** the
  interrupt key: it lets the REPL screen handle it.

- **`cpp_migration/src/ui/components/text_input.cppm:688`** — While a large-paste confirmation is
  pending, **all other keys are ignored** until it is resolved; Enter accepts and Esc cancels.

- **`cpp_migration/src/ui/components/custom_select.cppm:73`** — For more than 100K options, callers
  must populate `label`/`value`/`description`/`group` directly; the builtin filter is not viable at
  that scale.

- **`cpp_migration/src/ui/components/custom_select.cppm:410`** — `options.size()` may exceed
  `kMaxBitset`; large lists fall back to a non-bitset path.

- **`cpp_migration/src/ui/components/custom_select.cppm:124`** — When set, Enter/number keys do not
  fire selection (other keys still work).

- **`cpp_migration/src/ui/messages/scroll_keybindings.cppm:406`** — Tab / shift-tab are **navigation,
  never consumed by scroll**.

- **`cpp_migration/src/ui/messages/scroll_keybindings.cppm:547`** — An unrecognized printable
  character **clears stale prefixes** and does **NOT** get consumed.

- **`cpp_migration/src/ui/messages/scroll_keybindings.cppm:352`** — `s` and `fsm` are both mutated;
  the caller **MUST** own a single instance and not interleave calls.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:4229-4234`** — `Ctrl+L` forces a terminal redraw
  **WITHOUT mutating input** — it writes `CSI 2J` + `CSI H` and repaints; `input_text` and all other
  state are left untouched. Mutating input here is the bug this guards against.

- **`cpp_migration/src/ui/app_constructor.cpp:191-199`** — The redraw path mirrors
  `ink forceRedraw`: `ERASE_SCREEN` + `CURSOR_HOME`, then FTXUI repaints the full frame after the
  consumed keystroke. **Input state must not be mutated.**

- **`cpp_migration/src/ui/app_autocomplete.cpp:1196-1200`** — The cursor is reset to hidden **each
  frame** (TS Ink default behaviour); any active `declared_cursor` decorator on descendant elements
  may override it with a physical cursor anchor. This enables IME preedit text to appear inline at
  the insertion point and lets screen readers / magnifiers follow the input.

- **`cpp_migration/src/ui/common/declared_cursor.cppm:61-63`** — The reset happens **BEFORE** children
  render, so active declared cursors can override position/shape. FTXUI emits cursor-movement escape
  sequences, so ordering here is observable.

- **`cpp_migration/src/ui/common/declared_cursor.cppm:106`** — Declarations are **clamped to box
  bounds** so a stale declaration cannot send the cursor outside the frame.

- **`cpp_migration/src/ui/app.cppm:36-42`** and **`app.cppm:2110-2117`** — **macOS/BSD line-discipline
  workaround: disable VLNEXT.** `VLNEXT` (the "literal-next" char, `Ctrl+V` by default) is processed
  by the terminal line discipline **EVEN in non-canonical mode** (`ICANON` off) on macOS/BSD. FTXUI
  puts the terminal in non-canonical mode but does **NOT** clear `c_cc[VLNEXT]`, so every `Ctrl+V`
  becomes an `lnext` escape: a pair of `\x16` bytes collapses into a single literal `\x16`. Net
  effect: pressing `Ctrl+V` 8× registers only 4× — **half the image-paste keystrokes are silently
  dropped**. Fix: an RAII `DisableVlnext` in `RunApp`.

- **`cpp_migration/src/ui/app_constructor.cpp:160-162`** — A running query aborts immediately (TS
  `app:interrupt` is owned by `useCancelRequest`) — it **never** arms the exit double-press and never
  exits the app.

- **`cpp_migration/src/utils/clipboard.cppm:28-40`** — **osascript invocation gotcha (macOS).** Loom
  runs the terminal in raw mode. `std::system()` forks a child that **inherits fd 0 = the raw-mode
  terminal**; `osascript`, on detecting a TTY on stdin, takes a code path that misbehaves under raw
  mode and exits non-zero. So the *same* `osascript` command that works in a normal shell **fails
  silently inside the app** (`read_image_png()` returned `nullopt`, and the `[Image #N]` placeholder
  was erased ~700 ms later). Fix: run `osascript` **detached** — `fork()` + `setsid()` + `exec()` with
  stdin/stdout/stderr on `/dev/null`, so `setsid()` gives the child a new session with **no
  controlling terminal** and it cannot see the raw-mode TTY at all. This cannot be achieved through
  `std::system()`/`sh`.

- **`cpp_migration/src/utils/clipboard.cppm:44`** — A second user report ("Ctrl+V pressed 8×, only 4
  register") is recorded alongside the first, confirming this is a recurring class of bug.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:4531-4539`** — Ctrl+N / Ctrl+P navigate
  autocomplete suggestions with wraparound in both directions, and **both early-return when
  suggestions are empty**. The CPP port has no chord system, so TS's pending-chord gate is omitted.
  When `asn == 0` the event **intentionally falls through** — TS readline cursor/history movement is
  **not implemented** in this port, so do not assume the fall-through is dead code.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:3880-3881`** — One-shot guard: the bash/edit/write
  permission panels invoke **both** `on_abort` **AND** `on_decide(Abort)` on a single Esc; the TS
  contract is **one terminal reply**. The guard is what makes the second invocation harmless.

- **`cpp_migration/src/ui/permissions/permission_single_prompt.cppm:344-346`** — The same one-shot
  contract, stated as a prompt-state invariant: `callback_fired`; priority is (1) `on_abort` if
  present, (2) else `on_decide(...)` — **NEVER both**.


- **`cpp_migration/src/ui/dialogs/dialog_default_renderers.cppm:91-95`** — `EmitDecision` enforces the
  same contract: `on_abort` wins over `on_response` if set, and once the guard fires **any subsequent
  decision key is swallowed**. It returns `true` on the first successful emission; subsequent calls or
  a missing callback return `false` so Arrow/Tab still *appear* handled but nothing fires.

- **`cpp_migration/src/ui/dialogs/dialog_default_renderers.cppm:215`** — The payload is kept in sync
  with the state so the value is visible on render and to observers.

- **`cpp_migration/src/ui/dialogs/confirmation_dialog.cppm:16`** — The canonical confirmation body is
  `"This action cannot be undone."` — the self-documenting block for this dialog covers the rest.

- **`cpp_migration/src/ui/teams/swarm_collaboration_view.cppm:533`** — `opts.scroll_transcript` and
  `opts.auto_scroll` are declared but the view does not currently act on them.

- **`cpp_migration/src/ui/teams/live_teammates.cppm:107`** — Display strings are byte-truncated and
  then **backed off any UTF-8 continuation byte** (`10xxxxxx`) so a multi-byte character is never cut
  in half.

- **`cpp_migration/src/ui/messages/user_text_message.cppm:81-93`** — The paste truncation rule:
  unchanged if `<= 10_000` chars; otherwise **head 2500 + separator + tail**, and the newline count in
  the hidden region is computed from head-end to text-end.

### A.11 — Tools layer

- **`cpp_migration/src/tools/agent_runtime.cppm:1635-1637`** — **Migrated edge case.** An agent
  frontmatter `name` must be a **non-empty string**, not merely *truthy*: `name: 0` or `name: false`
  would otherwise round-trip as the strings `"0"` / `"false"` and **silently register an agent nobody
  can reference**.

- **`cpp_migration/src/tools/agent_runtime.cppm:1639-1644`** — **Migrated edge case.** TS silently
  skips when `description` is missing **OR is not a string**. The C++ must additionally differentiate
  "co-located reference markdown without `name:`" (**skip silently**) from "agent file with `name:`
  but no `description:`" (**log an error**). The differentiation happens in the *caller*
  (`load_agent_definitions_from_dir`) via the `get_parse_error()` fallback; this function returns
  `nullopt` regardless.

- **`cpp_migration/src/tools/agent_runtime.cppm:1647-1661`** — **Migrated edge case.** TS silently
  unescapes `\\n` sequences inside `description` strings that were YAML-escaped during parse; the C++
  replicates this.

- **`cpp_migration/src/tools/agent_runtime.cppm:1670-1672`** — **Migrated edge case.** The `model`
  field: TS silently lowercases and treats the value `"inherit"` **case-insensitively** as the string
  `"inherit"`. The C++ keeps the **raw value otherwise** so custom model aliases survive the
  round-trip — lowercasing everything would corrupt alias names.

- **`cpp_migration/src/tools/agent_runtime.cppm:2088`** and **`:2108`** — Co-located reference markdown
  (no `name:` frontmatter) is **silently skipped**; parse errors are reported only for files that
  **look like agents**. This matches TS `getParseError` semantics.

- **`cpp_migration/src/tools/agent_runtime.cppm:780-791`** — **Migrated edge case.** The legacy
  `:alias` suffix match requires **exactly one** candidate; an **ambiguous** suffix match (e.g. two
  code-explorer variants under different namespaces) returns `nullopt` **instead of the first match**.
  Returning `matches.front()` would silently pick an arbitrary agent.

- **`cpp_migration/src/tools/agent_runtime.cppm:793`** — No compatible match returns `nullopt`; the
  caller surfaces the error.

- **`cpp_migration/src/tools/agent_runtime.cppm:3661-3663`** — **Migrated edge case.** A worktree
  specified but **missing** must not **silently fall back to the parent cwd** at runtime — the missing
  directory is reported with a clear message so `AgentTool` can surface it.

- **`cpp_migration/src/tools/agent_runtime.cppm:3700-3702`** — **Migrated edge case.** Unbounded
  nesting is prevented. TS uses `MAX_SUBAGENT_DEPTH` *implicitly* through fork-recursion detection;
  the C++ tracks depth by traversing `parent_agent_id` and **rejects any agent whose chain exceeds 16
  ancestors**.

- **`cpp_migration/src/tools/agent_runtime.cppm:3734-3737`** — **Migrated edge case.** A fork directive
  supplied while `allow_fork` is false is an **error**, because the directive would otherwise be
  silently lost. TS errors here too.

- **`cpp_migration/src/tools/agent_runtime.cppm:3888-3899`** — **Migrated edge case.** Fork-type
  agents track the parent's **rendered system prompt** so that resuming a fork child without a stored
  fork context would lose the **byte-identical prompt prefix**. When the agent has no sidechain entries
  AND is marked a fork child, a **clear error** is surfaced instead of silently producing a divergent
  prompt cache key.

- **`cpp_migration/src/tools/agent_runtime.cppm:3907-3909`** — **Migrated edge case.** The worktree's
  mtime is **bumped** on resume so the **stale-worktree reaper** does not delete a just-resumed
  worktree before the first poll fires.

- **`cpp_migration/src/tools/agent_runtime.cppm:2448`** — The child-machine instruction string is
  **intentionally byte-stable across all fork children** so the prompt cache key matches.

- **`cpp_migration/src/tools/agent_runtime.cppm:3745`** — The actual LLM streaming loop is
  **intentionally NOT here** — it lives elsewhere; this module must not grow a second copy.

- **`cpp_migration/src/tools/agent_sub_utils.cppm:3031-3037`** — **Migrated edge case.** Resuming from
  sidechain entries drops assistant messages with any `tool_use` that never received a `tool_result`
  (mirrors TS `filterUnresolvedToolUses`). Two filters with **different strictness** are applied:
  `filter_resume_unresolved_tool_use_messages` handles the case where an assistant's tool_uses are
  ***all*** unresolved, whereas `filter_incomplete_tool_calls` is **stricter** and drops an assistant
  whenever ***any*** tool_use lacks a result. Do not consolidate them into one.

- **`cpp_migration/src/tools/agent_constants.cppm:17-19`** — `LEGACY_AGENT_TOOL_NAME = "Task"` is kept
  for backward compatibility with **persisted permission rules, hooks, and resumed sessions**.
  Removing or renaming it invalidates data on disk.

- **`cpp_migration/src/tools/agent_constants.cppm:24-30`** — "One-shot" built-in agents (`Explore`,
  `Plan`) run once and return a report — the parent **never** calls `SendMessages` to continue them.
  The wrapper therefore **intentionally skips** the `agentId` / `SendMessage` / usage trailer, saving
  ~135 chars × millions of Explore runs per week. Adding the trailer back is a token-cost regression.

- **`cpp_migration/src/tools/agent_color_manager.cppm:7-9`** — The **general-purpose ("main thread")
  agent intentionally has no assigned color** to distinguish it from sub-agents.


- **`cpp_migration/src/tools/should_use_sandbox.cppm:46-52`** — The TS version takes a
  `Partial<SandboxInput>` (command + `dangerously_disable_sandbox`); the C++ `SandboxInput` mirrors
  that shape **exactly** via `std::optional`s, so an absent field is distinguishable from a defaulted
  one.

- **`cpp_migration/src/tools/readonly_validation.cppm:648-661`** — The TS version invokes
  `sedCommandIsAllowedByAllowlist` through an additional callback. The C++ replicates only the
  "`-n` + `p`/`num,num p` patterns" check inline with a **conservative** rule. It is **stricter than
  the TS allowlist but strictly safer** — do not loosen it to reach parity, because the TS allowlist
  is larger than what is re-implemented here.

- **`cpp_migration/src/tools/readonly_validation.cppm:274-282`** — Only the **most common /
  security-critical** safe-flag entries were carried over. The TS file lists ~60 commands with 1000+
  flags total; the C++ covers xargs, sed, sort, grep, rg, fd, sha*sum, md5sum, date, hostname, file,
  netstat, ps, base64, ss, tput, lsof, pgrep, tree, info, man, help, jq, plus git read-only commands.
  **Missing commands gracefully fall back to the regex path or manual approval** — this is the
  designed degradation, not a bug.

- **`cpp_migration/src/tools/readonly_validation.cppm:303-306`** — TS rejects tokens whose type is not
  `'string'` (bash operators) at this point; the C++ `simple_shell_tokenize` already **strips
  operators** by splitting on `&&`/`||`/`;`/`|`, so the check is unnecessary here. If the tokenizer
  changes, this assumption breaks.

- **`cpp_migration/src/tools/readonly_validation.cppm:79`** — The additional-validation callback exists
  because e.g. `ps` must not carry arbitrary flags.

- **`cpp_migration/src/tools/path_validation.cppm:117-127`** — **SECURITY.** Most commands (`rm`,
  `cat`, `touch`, …) stop parsing options at `--` and treat **all** subsequent arguments as positional,
  **even if they start with `-`**. A naive `!arg.starts_with('-')` filter drops these, causing **path
  validation to be silently skipped** for payloads like `rm -- -/../.loom/settings.local.json`. There
  the argument starts with `-`, the naive filter drops it, validation sees **zero paths** → returns
  passthrough → **the file is deleted without a prompt**. With `--` handling the path IS extracted and
  validated.

- **`cpp_migration/src/tools/path_validation.cppm:104-112`** — The shell-command parsing helpers
  **deliberately live inside this module** rather than a hypothetical `bash.commands` module, because
  they are very tightly coupled to the `--` end-of-options behaviour each path extractor relies on.
  Keeping them here avoids circular-import problems.

- **`cpp_migration/src/tools/path_validation.cppm:784-792`** — **Every historical and current
  config-home name must be listed** in the write-blocking guard. The guard protects credentials and
  settings, so a directory that **USED TO** hold them is exactly as sensitive as one that does now;
  **omitting a legacy name would silently unprotect a real `~/.<old>` directory that still exists on
  disk**. The bare-name variants (no leading dot) are the XDG-style roots used on Linux.

- **`cpp_migration/src/tools/path_validation.cppm:287-289`** — **KEEP IN SYNC** with
  `stripSafeWrappers` (the text-based version) in the bash-permissions module and with the
  wrapper-stripping logic in the semantic checker. Three implementations of the same wrapper list
  exist and must agree.

- **`cpp_migration/src/tools/path_validation.cppm:1033-1038`** — The TS source accepts pre-parsed
  `astRedirects` / `astCommands` **or** falls back to string-based splitting. The C++ port uses the
  **string-based fallback** for simplicity; a second overload can be added later without breaking call
  sites.

- **`cpp_migration/src/tools/path_validation.cppm:1077-1080`** — `split_compound_command` is
  **deliberately simple**; consumers that need full AST semantics should use `bash_ast`. Do not add
  progressively more shell parsing here.

- **`cpp_migration/src/tools/path_validation.cppm:120`** — A naive `!arg.starts_with('-')` filter causes
  validation to be silently skipped; see the SECURITY note above.

- **`cpp_migration/src/tools/sed_edit_parser.cppm:274-275`** — `-i` / `--in-place` is **intentionally
  consumed but not interpreted** here — it matters at the `SedEditInfo` layer below, which is where
  in-place semantics are decided.

- **`cpp_migration/src/tools/destructive_command_warning.cppm:44-51`** — The patterns are `std::regex`
  rather than string `find()` calls because the TS source uses JS `RegExp` with **word boundaries,
  lookaheads, alternations and the `icase` flag** that cannot be expressed reliably with plain
  substring search. They are ordered **by sensitivity**: git data-loss ops first, then git
  safety-bypass, then file deletion, then database, then infrastructure.

- **`cpp_migration/src/tools/sed_validation.cppm:281`** — `-n` / `--quiet` / `--silent` (or the
  combined form containing `n`) is **required** — without it `sed` can write, so the read-only
  classification must fail.

- **`cpp_migration/src/tools/sed_validation.cppm:13`** — Path-scope permission checks are delegated to
  `bash_validation` (already implemented) rather than duplicated.

- **`cpp_migration/src/tools/script_primitives.cppm:505-510`** — Each primitive's `execute()` receives
  args in order matching `parameters`; args are JSON-decoded strings; the in-process implementations
  **deliberately avoid any subprocess work to match the TS contract**.

- **`cpp_migration/src/tools/script_primitives.cppm:610-613`** — The `Agent` primitive only captures
  the **"in-process" behaviour** mandated by the TS side-effect-free primitive contract; full
  orchestration lives in the coordinator module and real dispatch routes there.

- **`cpp_migration/src/tools/script_primitives.cppm:10`** — `NotebookEditTool` is a minimal JSON cell
  editor (yyjson), not a full notebook implementation.

- **`cpp_migration/src/tools/file_edit_tool.cppm:506-513`** — Two ordering/atomicity constraints.
  (1) The file-history hook runs **BEFORE** the staleness check, like TS. (2) The critical section
  (read + staleness check + write) **deliberately uses only synchronous `std::filesystem`**, because
  async yields between read and write would **break atomicity**.

- **`cpp_migration/src/tools/file_edit_tool.cppm:565`** — Step 8 refreshes `readFileState` so that
  **stale subsequent writes abort**.

- **`cpp_migration/src/tools/file_edit_prompt.cppm:95-104`** — The full TS `userFacingName()` also
  checks `getPlansDirectory()`; the C++ **requires callers to pass the plans-dir prefix explicitly**.
  When empty, it falls back to "Update" semantics — so a caller that forgets the prefix gets the wrong
  header for plan files.

- **`cpp_migration/src/tools/mode_validation.cppm:89-97`** — TS uses `splitCommand_DEPRECATED` (which
  handles `&&`/`||`/`;`/`|`/`|` bash operators). For the **mode-validation** path a full shell parser
  is **not needed** — only the "base command" (first whitespace-delimited token) of each subcommand
  is required, which is the same precision TS achieves when it later calls `.split(/\s+/)`.

- **`cpp_migration/src/tools/web_fetch_tool.cppm:220`**, **`web_search_tool.cppm:335`**,
  **`bash_tool.cppm:1252`**, **`agent_tool.cppm:1385`**, **`file_write_tool.cppm:372`**,
  **`file_read_tool.cppm:601`**, **`grep_tool.cppm:186`**, **`glob_tool.cppm:191`** — Every tool
  exposes a `Factory:` that wraps the concrete tool as `ITool`, explicitly described as
  "adapts Result types across modules". The wrapping is the cross-module type seam; constructing a
  tool directly bypasses it.

- **`cpp_migration/src/tools/tool.cppm:298`** — `register_tool` accepts a pre-constructed tool and
  **bypasses the concept check across module boundaries** — this is why the explicit `ITool` factories
  above exist.

- **`cpp_migration/src/tools/tool.cppm:339`** — `get_visible_definitions()` returns only **non-hidden**
  tools for the API request body. Hidden commands/tools must be reached another way.

- **`cpp_migration/src/tools/file_read_tool.cppm:69`** — Blocked device paths are listed explicitly;
  "we should **never** read" them.

- **`cpp_migration/src/tools/mcp_classify.cppm:759-771`** — Collapse classification order: (1) SEARCH
  allowlist → `AutoCollapse`; (2) READ allowlist → `AutoCollapse`; (3) else (mutating/unknown) →
  `AlwaysShow`; (4) **error results are always shown regardless** — **errors are never collapsed**,
  because the user needs to see what went wrong.

- **`cpp_migration/src/tools/mcp_tool.cppm:1055-1059`** — **TS PARITY (2026-07-04).** The structured
  `ContentItem` array is preserved instead of being flattened to a single string: MCP servers may
  return mixed text+image content blocks, and flattening **loses images** and concatenates text blocks
  with `"\n"`, which then needs the `try_extract_result_summary_text` workaround.

- **`cpp_migration/src/tools/runtime_registry.cppm:1436-1437`** — When a configured computer-use MCP
  server **rejects** an action, the code fails closed with the routing error rather than
  **silently running the wrong (local) backend**.

- **`cpp_migration/src/tools/runtime_registry.cppm:1966-1971`** — After `team_create`, the process must
  **establish itself as the leader** by setting `LOOM_TEAM_NAME` and the leader team name. Without
  this, the live teammate projection / pane observer / leader permission inbox **never activate**
  because `get_team_name()` stays empty.

- **`cpp_migration/src/tools/runtime_registry.cppm:2713-2718`** — The simplified property model
  **cannot represent nested MCP input schemas**. The verbatim schema is carried separately by
  `NativeMcpRuntime` and surfaced at request serialization time; `def.input_schema` is left **empty**
  here on purpose. Populating it with a lossy simplified schema would shadow the verbatim one.

- **`cpp_migration/src/tools/runtime_registry.cppm:450-453`** — `parse_lsp_action`'s canonical action
  strings **mirror `lsp_action_name()` in `lsp_tool.cppm`**. Without these mappings the runtime
  registry's `execute_lsp_tool` would **silently fall through to `LspAction::Symbols`** for the newer
  actions — a silent wrong-answer, not an error.

- **`cpp_migration/src/tools/runtime_registry.cppm:2185-2189`** — Feature-gated **stub** tools exist so
  that when a feature flag is enabled the tool is registered and returns a meaningful
  "not yet implemented" message **rather than crashing**.

- **`cpp_migration/src/tools/runtime_registry.cppm:1075-1084`** — Three tools are registered
  **unconditionally** in C++ where TS gates them (`repl` is ant-only in TS; `schedule_cron` is behind
  `AGENT_TRIGGERS`; the remote-trigger tool's runtime behaviour is controlled by
  `LOOM_REMOTE_TRIGGER_COMMAND`), because the C++ implementations actually work.

- **`cpp_migration/src/tools/computer_use.cppm:841-845`** — Anthropic computer-use returns a **FRESH
  screenshot after every action**, not just the explicit screenshot action — the model is otherwise
  **blind to the result of a click/keystroke**. A full-screen capture is therefore attached to every
  successful input action's result. If capture is unavailable the action still returns success
  (text-only) and the caller decides whether that is fatal for the current platform.

- **`cpp_migration/src/tools/built_in_agents.cppm:41`** — Absolute-path + emoji guidance is **appended
  at runtime**, not baked into the agent prompt template.

- **`cpp_migration/src/tools/built_in_agents.cppm:460`** — Empty values keep the prompt template
  intact rather than collapsing it.

- **`cpp_migration/src/tools/repl_tool.cppm:1-6`** — This **replaces the previous `popen`-based
  "one-shot" REPL helper** with a true persistent session manager: interpreters (python3 / node / bun /
  ruby) are kept alive as child processes via `posix_spawn` with bidirectional pipes, and state is
  preserved across evaluations. The old one-shot behaviour is gone.

- **`cpp_migration/src/tools/bash/impl_bash.cppm:166-169`** — The `sandbox-exec` profile is
  **deliberately conservative for P0**: it allows basic filesystem reads/writes, HTTPS outbound, and
  common IPC endpoints, and forbids raw disk access and privileged syscalls. **Hardening happens in a
  later pass** — this is a known, intentionally incomplete profile.

- **`cpp_migration/src/tools/bash_helpers.cppm:234`** — Whitespace is trimmed first because the caller
  contract mirrors the TS `s.trim().match(RE)`.

- **`cpp_migration/src/tools/bash_result_formatting.cppm:349-352`** — `cwd_reset_warning` /
  `sandbox_violations` / `stdout` / `stderr` bodies are **deliberately NOT in the header** — they are
  emitted as separate blocks by the UI layer. Callers needing them formatted before display use
  `extract_all_stderr_meta()`.

- **`cpp_migration/src/tools/bash_tool.cppm:902`** — "silent" here means the command is expected to
  produce no output, not that it is suppressed from rendering.

- **`cpp_migration/src/tools/command_semantics.cppm:280`** — The **last** command in a pipeline is what
  determines the shell exit code.

- **`cpp_migration/src/tools/command_semantics.cppm:229`** — Anonymous namespaces cannot be exported;
  this constrains file layout when a helper needs module-visible linkage.

- **`cpp_migration/src/tools/runtime_team_shared.cppm:534`** — Any step that fails is
  **silently absorbed** — the team *record* is already authoritative.

- **`cpp_migration/src/tools/runtime_shared_utils.cppm:9`** — Hoisting these helpers into their own
  module **breaks the shared-helper coupling** they were extracted to preserve.

- **`cpp_migration/src/tools/runtime_shared_utils.cppm:85`** — The sanitizer replaces any character
  that is not alphanumeric, a dash, an underscore or a dot with `_`.

- **`cpp_migration/src/tools/lsp_tool.cppm:548`** — `CallHierarchyItem` is re-emitted under the `item`
  key to preserve the TS wire shape.

- **`cpp_migration/src/tools/feature_flags.cppm`** (whole file) — Records which TS `src/tools.ts`
  feature gates map to which C++ flags; each entry names the TS gate (e.g. `KAIROS ||
  KAIROS_PUSH_NOTIFICATION` for the push-notification tool, `USER_TYPE === 'ant'` for ant-only tools,
  `NODE_ENV === 'test'` for the testing-permission tool). These mappings are the TS-side intent behind
  the C++ flags.

### A.12 — Skills

- **`cpp_migration/src/skills/lorem_ipsum.cppm:14-17`** — **The TS skill does NOT actually open with
  the canonical "Lorem ipsum dolor sit amet…" phrase** — every word is random-sampled from
  `ONE_TOKEN_WORDS`. This is **preserved exactly**, because inserting the canonical opening would
  **change the token-count contract** the skill exists to satisfy.

- **`cpp_migration/src/skills/lorem_ipsum.cppm:85-86`** — The one-token word list has been
  **API-verified** to occupy exactly one token each; sentences are 10-20 words.

- **`cpp_migration/src/skills/lorem_ipsum.cppm:341-348`** — TS: the ant-only gate **silently does
  nothing** (early return). The C++ keeps parity but sets `ok = true` with empty output and surfaces a
  short diagnostic hint.

- **`cpp_migration/src/skills/lorem_ipsum.cppm:8-21`** — The C++ adds extensions not in TS
  (`generate_paragraphs`, `generate_words`, `generate_chars`, `generate_json_placeholder`,
  `generate_code_block`, `generate_csv`, natural-language number parsing). These are **additions**, not
  ports — do not cite them as TS parity.

- **`cpp_migration/src/skills/verify_content.cppm:8-15`** — The TS `/verify` skill is
  **intentionally narrow in scope**: it runs **the project itself** (not the test suite), focuses on
  CLI binaries or HTTP servers, and produces a side-by-side "expected vs. observed" report. UI
  components (React renderers) are **intentionally omitted** — a Phase 4 concern.

- **`cpp_migration/src/skills/verify_content.cppm:41`** — HTML `<!-- comments -->` are stripped with
  the **same pipeline** used in `loom_api_content`.

- **`cpp_migration/src/skills/loom_api_content.cppm:32-35`** — Model identifiers substitute into
  `{{OPUS_ID}}`-style placeholders inside prompt fragments and must be **kept in sync with the TS
  `SKILL_MODEL_VARS`**; the C++ equivalent is a plain `constexpr` map.

- **`cpp_migration/src/skills/loom_api_content.cppm:187-190`** — **The TS source references docs that
  live in an inlined 247 KB `SKILL_FILES` blob.** At runtime the C++ builds the **same routing table**
  but delegates resolution to `WebFetchTool` against the URLs in `shared/live-sources.md` when a
  concrete doc body is needed — the bundled docs are not carried.

- **`cpp_migration/src/skills/loom_api_content.cppm:242-246`** — The TS original **slices** the
  When-to-Use-WebFetch + Common-Pitfalls sections out of `SKILL.md` by **heading anchor**; the C++
  embeds them **verbatim** so the prompt sections stay reachable even when the source `.md` snapshot is
  stripped. A future re-slice must keep byte-identical content or the prompts drift.

- **`cpp_migration/src/skills/bundled.cppm:261-267`** — This skill **used to be registered as
  `"stuck"` (v1.0)**. The `/stuck` slash command in TS is about **diagnosing OTHER sessions on the same
  machine**, so the generic self-unstuck advice was moved to a **separate discoverable name**
  (`"self-unstuck"`) so neither behaviour is lost. Re-merging the names re-creates the ambiguity.

- **`cpp_migration/src/skills/bundled/skill_keybindings.cppm:450-453`** — The skill name is
  **`"keybindings-help"` (NOT `"keybindings"`)**. The root-level `cc.skills.keybindings` module
  provides a **simple shortcut reference sheet** under the name `"keybindings"`. Two different skills,
  two different names.

- **`cpp_migration/src/skills/bundled/skill_keybindings.cppm:26`** and **`:227`** — The static
  reference data and the prompt sections are **kept in sync with TS** (`keybindings/schema.ts` and the
  `SECTION_*` constants) by hand.

- **`cpp_migration/src/skills/bundled/loom_in_chrome.cppm:41`** — The base Chrome system prompt is
  **kept in sync with the TS source file** by hand.

- **`cpp_migration/src/skills/load_skills_dir.cppm:366-376`** — TS checks a `pluginOnlyPolicy`
  **setting**; the C++ checks an **environment variable** (`LOOM_PLUGIN_ONLY_SKILLS`) instead. This is
  a deliberate substitution, not a bug — but it means settings-file configuration will not work.

- **`cpp_migration/src/skills/load_skills_dir.cppm:263-265`** — **Shell execution** (`` !`cmd` `` blocks
  in skills) is **deferred to the caller**, because it requires the full tool-use context and
  permission system. Skills therefore cannot self-execute shell at load time.

- **`cpp_migration/src/skills/load_skills_dir.cppm:1575`**, **`:2254`**, **`:2261`** — Locking
  invariants: `dynamic_state().mutex` is **already held by the caller**; `all_skills()` already holds
  it. Re-acquiring is a deadlock.

- **`cpp_migration/src/skills/skill.cppm:380-384`** — Skill discovery is triggered from file tools via
  a **file-access hook**, because `cc_tools` **cannot depend on `cc_skills`** (circular dependency).
  The hook is the decoupling seam.

- **`cpp_migration/src/skills/skill.cppm:427`** — Skill discovery is **best-effort; it must never fail
  the file operation** that triggered it.

- **`cpp_migration/src/skills/skill.cppm:74`** — The `Skill` concept is the contract every skill
  implementation must satisfy.

### A.13 — Query engine, attachments, compaction

- **`cpp_migration/src/query/query_engine.cppm:355-359`** — **AT-02.** Materialized `@`-mention file
  attachments are appended to the user message content **after the text block** so the model actually
  sees file contents. Before this fix the C++ port passed `"@path"` **literally**, making `@` a
  **no-op for the model** — a silent capability loss, not an error.

- **`cpp_migration/src/query/query_engine.cppm:1794-1796`** — `append_session_summary` is
  **best-effort**: a write failure **must not break an in-memory compaction that already succeeded**.

- **`cpp_migration/src/query/query_engine.cppm:3145-3150`** — `width`/`height`/`size_bytes` are **not
  returned** by the tool-use executor (it forwards only `format`/`media_type`/`data`), so they are left
  `std::nullopt`. The user-facing renderer falls back to `"<no metadata>"` and an ASCII thumbnail
  seeded from the base64 payload. Do not assume those fields are populated.

- **`cpp_migration/src/query/query_engine.cppm:820`** — Recent messages are kept, but **stale compact
  boundaries from prior compaction chains are dropped**.

- **`cpp_migration/src/query/query_engine.cppm:3362-3363`** — The auto-memory extraction runs
  **detached** so it **never blocks the interactive loop**.

- **`cpp_migration/src/query/query_engine.cppm:2114`** — The wire backend is constructed per request so
  a config change **cannot leave a stale backend behind**.

- **`cpp_migration/src/query/config.cppm:38-41`** — A `QueryDeps` DI-seam struct previously lived here
  but was declared (2026-06 baseline) with **zero references anywhere** — dead code, removed
  2026-06-15. **Do not revive the unused prototype**; re-introduce a dependency-injection abstraction
  only when an actual consumer needs it.

---

## B. Non-obvious constraints — a rule a future maintainer would violate without knowing

### B.1 — Permission system

- **`cpp_migration/src/hooks/permission_resolver.cppm:29-35`** — The `Decision` enum's **numeric
  values must NOT be reordered**. The persisted permission cache (`CacheToJson`) writes the integers,
  so **any shift would invalidate every on-disk permission grant**. The values are pinned by a
  "Task #58 contract": `AllowOnce=0, AlwaysAllow=1, Deny=2, AlwaysDeny=3, Abort=4`. Adding a new
  decision requires appending, never inserting.

- **`cpp_migration/src/hooks/permission_resolver.cppm:280-283`** — `CacheAlways` records a cache entry
  **only** for `AlwaysAllow` / `AlwaysDeny`. Any other `Decision` is a **no-op by design**
  (defense-in-depth): the caller should not pass other values, and **silently ignoring keeps the API
  robust**. Passing `AllowOnce` expecting it to be cached will do nothing.

- **`cpp_migration/src/hooks/permission_resolver.cppm:327-335`** — `CacheFromJson` uses a **minimal
  hand-rolled recursive-descent parser** and **intentionally avoids pulling in the full JSON module**,
  so the resolver can be used by the **earliest startup paths** (before the heavy utils layer is
  initialised). Swapping in the shared JSON parser re-introduces a startup-ordering hazard.

- **`cpp_migration/src/hooks/permission_resolver.cppm:385-393`** — `\uXXXX` escapes are accepted
  **only for ASCII** — permission rules never need full Unicode — and anything non-ASCII is written
  back **verbatim as four hex digits so the round-trip stays lossless**. Decoding non-ASCII here would
  corrupt the round-trip.

- **`cpp_migration/src/hooks/permission_resolver.cppm:271-277`** — When nothing else matches the
  resolver returns `Decision::Deny` so the gate can **short-circuit the interactive prompt** when the
  caller has not provided one; the gate **re-prompts** when an interactive hook is available. Returning
  `AllowOnce` as a "safe default" would be a privilege escalation.

- **`cpp_migration/src/hooks/permissions.cppm:199-208`** — In plan mode, **all non-read tools are
  silently denied**. Read-only is decided by `invocation.is_read_only || read_only_tools_.contains(...)`;
  relying on the flag alone is insufficient because not every read-only tool sets it.

- **`cpp_migration/src/hooks/permissions.cppm:187`** — `read_only_tools_` is the explicit set of tools
  that **never** modify the filesystem or external state; adding a mutating tool to it silently
  disables plan-mode protection for that tool.

- **`cpp_migration/src/hooks/permission_resolver.cppm:112`** — `payload` is a **tool-specific
  free-form** field (the raw Bash command, or the file path); consumers must know the tool to interpret
  it.

- **`cpp_migration/src/hooks/tool_permission_gate.cppm:5`** — The gate exists so that when a request
  **cannot** be auto-approved, a **user-visible** permission prompt is produced; it must not swallow
  the request.

- **`cpp_migration/src/hooks/tool_permission_gate.cppm:34`** — When the resolver **cannot**
  auto-approve, the gate invokes a caller-supplied callback.

- **`cpp_migration/src/services/mcp/channel_permissions.cppm:296-308`** — Filtering MCP clients to
  permission-relay candidates requires **ALL THREE** conditions: (1) connected (`state == Ready`),
  (2) in the session's `--channels` allowlist, (3) declares **BOTH** capabilities
  `loom/channel` **AND** `loom/channel/permission`. The second capability is the server's **explicit
  opt-in** — a relay-only channel **must never** become a permission surface by accident.

- **`cpp_migration/src/services/mcp/channel_permissions.cppm:14-15`** — The permission reply arrives as
  a **structured event** (`notifications/loom/channel/permission` with `{request_id, behavior}`).
  **CC never sees the reply as text** — approval is not parsed from prose.

- **`cpp_migration/src/services/mcp/channel_permissions.cppm:350-352`** — The text-reply parser
  (`"yes tbxkq"` / `"no tbxkq"`) is **NOT the production path**. In production the **SERVER** parses the
  reply and emits the structured event; this utility exists for **testing and standalone tooling**, and
  is exported so plugins can reuse the exact same logic.

- **`cpp_migration/src/services/mcp/channel_permissions.cppm:543`** — Setting the same identity
  **overwrites** the permission rule.

- **`cpp_migration/src/utils/settings_rules.cppm:320`** — A "plugin-only" key **cannot** be set in
  user or project settings; only the plugin scope is legal.

- **`cpp_migration/src/utils/powershell_parser.cppm:313`** — A specific command set is
  **never** suggested as a wildcard prefix in the permission dialog, because doing so would grant more
  than the user intends.

### B.2 — The one-shot / exactly-once callback family

- **`cpp_migration/src/services/api/sse_client.cppm:250`** — `on_final` is emitted **exactly once**.
  See the divergence note in A.3 for why it is bound to `curl_easy_perform` rather than
  `message_stop`.

- **`cpp_migration/src/services/api/sse_client.cppm:67-72`** — `on_event` is called **once per complete
  SSE event**, and `on_final` **exactly once per request**.

- **`cpp_migration/src/services/mcp/transport_stdio.cppm:137-141`** — `on_message` fires **exactly
  once** for every newline-delimited message received; `on_exit` fires **exactly once** when the reader
  thread terminates, **whether because of** an error, a normal stop, or EOF.

- **`cpp_migration/src/services/mcp/transport_stdio.cppm:156`** — Calling `Start()` twice without an
  intervening stop is not supported.

- **`cpp_migration/src/ui/dialogs/cost_threshold_dialog.cppm:73-75`** — `on_done` is invoked
  **EXACTLY once** when the user acknowledges via Enter, Escape, or a character shortcut.

- **`cpp_migration/src/ui/messages/message_pipeline.cppm:95-106`** — The `DedupTracker` invariant: a
  given content-block `index` transitions **exactly once**: `NotSeen → Start → Open → Delta* → Stopped`
  (terminal). A duplicate `Start` (e.g. a reconnect replay) for an already-`Stopped` index is
  **SILENTLY DROPPED** (returns `false`). A duplicate `Stop` is also dropped — this specifically
  prevents the `streaming_tools[id].complete` flag from **bouncing true→false→true** when the server
  replays events.

- **`cpp_migration/src/ui/messages/message_pipeline.cppm:169`** — A tool that has already been `End`ed
  **cannot restart its Start**.

- **`cpp_migration/src/services/lsp/manager.cppm:288-292`** — Passive LSP notification handlers are
  registered **exactly once per successful initialization** (the manager pointer is non-null and the
  mutex is held at that point). Registering per-connection double-reports diagnostics.

- **`cpp_migration/src/services/lsp/passive_feedback.cppm:349`** — Recording **merges** with existing
  items; it **does not clear** them.

- **`cpp_migration/src/services/lsp/passive_feedback.cppm:490`** — Per-server errors are isolated so
  one failing server **cannot break the notification** pipeline for the others.

- **`cpp_migration/src/ui/permissions/permission_single_prompt.cppm:344-346`** — See A.10: exactly one
  terminal callback per prompt.

### B.3 — MCP / wire-format fields

- **`cpp_migration/src/services/mcp/types.cppm:384-390`** — The `inputSchema` object is preserved
  **verbatim** because it carries nested shapes (objects, arrays, `$ref`, vendor keys) the **simplified
  property model cannot represent**; it is serialized verbatim into the API request body. Do not
  round-trip it through the simplified model.


- **`cpp_migration/src/server/types.cppm:6-13`** — `cc::server::detail` already holds
  `DirectQueryRequest` / `DirectQueryResult` / `DirectPermissionRequest` / `DirectPermissionRule` /
  `DirectPermissionDirectory` / `DirectPermissionSessionState` as **route-local helpers**. Those are
  **intentionally kept internal** to `cc.server.server_routes` (detail namespace, **different shape** —
  they carry cancel flags, filesystem paths, etc.). The types in `server/types.cppm` are the
  **canonical HTTP DTOs**; future refactors are expected to make `server_routes` **convert between**
  its internal helpers and these public structs.

- **`cpp_migration/src/server/types.cppm:229`** — Timestamps are **epoch milliseconds**, and
  `expires_ms = 0` means **"never expires"** — not "expired at epoch".

- **`cpp_migration/src/services/mcp/in_process_transport.cppm:24-29`** — The raw JSON body is passed
  through **unchanged** so callers can parse it with their own JSON library (matching the opaque
  `JSONRPCMessage` contract in the TS SDK). Parsing here would change the contract.

- **`cpp_migration/src/services/mcp/in_process_transport.cppm:41-43`** — `send()` **errors** when
  closed, otherwise forwards to the peer's `on_message`; `close()` marks **both** sides closed and
  invokes `on_close` on **both**.

- **`cpp_migration/src/services/mcp/in_process_transport.cppm:50`** — `start()` is a **no-op** for
  parity with TS's async `start()` which resolves immediately.

- **`cpp_migration/src/services/mcp/transport_stdio.cppm:77`** — The parsed JSON-RPC payload is
  **intentionally kept simple**; `string` ids are ignored (the dispatcher uses ints).

- **`cpp_migration/src/services/mcp/oauth_port.cppm:31-37`** — The redirect-port range is RFC 6056
  ephemeral (`49152-65535`). **Windows reserves 49152-65535**, so the TS original uses `39152-49151`
  there; the C++ uses the non-Windows range and **does not branch on platform** (the C++ migration does
  not currently detect Windows). This will need a platform branch if Windows is ever targeted.
  `kDefaultOAuthPort = 8912` is the final fallback after random probing fails and is kept as a named
  constant so the behaviour stays discoverable.

- **`cpp_migration/src/services/mcp/channel_notification.cppm:484-489`** — For `server-kind` channels,
  the allowlist schema is `{marketplace, plugin}` — **a server entry can never match**. Without the
  explicit rejection, `--channels server:plugin:foo:bar` would match a plugin's **runtime name** and
  register with **no allowlist check**.

- **`cpp_migration/src/services/mcp/channel_notification.cppm:283-288`** — Channel-entry matching
  returns the entry so callers can read its `kind` — that kind is **the user's trust declaration, not
  inferred from runtime shape**. Do not derive the kind from the server's capabilities.

- **`cpp_migration/src/services/mcp/channel_notification.cppm:293-294`** — Channel names are split
  **unconditionally**: for a bare name like `'slack'`, `parts` has size 1 and the plugin-kind branch
  correctly never matches.

- **`cpp_migration/src/services/mcp/channel_notification.cppm:965-976`** — The C++ adds an **explicit
  health monitor** because its transport layer is thinner than the TS SDK's. This monitor **pings**
  servers that have notifications capability, tracks consecutive failures, and emits `ServerError`
  **after 3 failures** so the UI can show degraded state. In TS this is distributed across the SDK's
  SSE reconnect/backoff and the connection manager's refresh workers.

- **`cpp_migration/src/services/mcp/channel_notification.cppm:1258-1261`** — The C++ **provides
  explicit integration functions** so the connection manager (or any caller) can wire channel
  notifications to server events; TS's connection manager emits lifecycle events that the notification
  system subscribes to. The C++ has no automatic subscription.

- **`cpp_migration/src/services/mcp/channel_notification.cppm:1248`** — The health monitor is a
  **process-wide singleton**.

- **`cpp_migration/src/services/mcp/channel_notification.cppm:1346`** — A template is used to avoid a
  **hard dependency** on the concrete manager type.

- **`cpp_migration/src/services/mcp/xaa.cppm:215`** — Unknown token types are redacted by
  **constructing a safe representation** rather than echoing the raw value.

- **`cpp_migration/src/services/mcp/at_mention_handler.cppm:117`** — Responder errors are swallowed so
  **a buggy UI hook cannot take down the** MCP path.

### B.4 — LSP

- **`cpp_migration/src/services/lsp/LSPServerManager.cppm:829-837`** — Every **production** LSP server
  gets a `publishDiagnostics` observer, and it is **observability-only**: the TS handler logs, and **a
  server push is NOT a user action and must not record acceptance/rejection feedback**. It is
  registered on **each instance** (rather than only on the `LspManager` singleton) so managers created
  via `create_lsp_server_manager()` — the ones `LspTool` owns — are covered too.

- **`cpp_migration/src/services/lsp/diagnostic_registry.cppm:676-702`** — `diagnostics_to_json_array`
  must emit elements with a **NUMERIC severity (1-4)**, plus `range.start`/`end` line/character,
  `message`, and optional `source`/`code`. **severity is always present and numeric** specifically so
  `parse_diagnostics` **never falls back to its missing-severity default (Info)** — an absent severity
  would silently downgrade every error to Info.


- **`cpp_migration/src/services/lsp/client.cppm:117-126`** — The client-local diagnostic struct is
  named `LspClientDiagnostic`, **renamed from `Diagnostic`**, to avoid colliding with
  `cc.services.lsp.diagnostic_registry`'s exported `cc::services::lsp::Diagnostic` — **both modules
  export into the same namespace**. Re-using the name re-creates an ODR collision.

- **`cpp_migration/src/services/lsp/client.cppm:1516-1526`** — LSP allows `code` to be a **string OR a
  number**; the TS preserves it via `String(code)`. The C++ therefore keeps `code_text` (raw, preferred
  when present) and populates the **numeric `code` only when the string is actually numeric**. Dropping
  `code_text` loses codes like `"TS2345"` / `"strictNullChecks"`.

- **`cpp_migration/src/services/lsp/client.cppm:728-730`** — When a diagnostic registry is injected,
  incoming `publishDiagnostics` notifications are upserted/cleared there **in addition to** the legacy
  callback — not instead of it.

- **`cpp_migration/src/services/lsp/LSPServerInstance.cppm:307-313`** — `send_request<T>` currently
  only materialises `T = std::string`; a `static_assert` **fails the build** for any other `T`. The
  rationale is explicit: **failing to compile is preferable to silently returning `T{}`**. A future `T`
  must grow a real branch above the assert.

- **`cpp_migration/src/services/lsp/manager.cppm:313-319`** — `refresh_capabilities` is called after
  plugin caches are cleared so newly-loaded plugin LSP servers are picked up. Separately: if
  `initialize()` was never called, do **not** start the manager now.

- **`cpp_migration/src/services/lsp/passive_feedback.cppm:293`** — The feedback file lives at
  `~/.loom/lsp-passive-feedback.json`.

- **`cpp_migration/src/services/lsp/diagnostic_registry.cppm:548`**, **`:585`** — The delivered-diagnostics
  LRU has a fixed maximum; exceeding it evicts.

### B.5 — Swarm / team protocol

- **`cpp_migration/src/utils/swarm_helpers.cppm:516-532`** — **The permission-sync protocol shape is
  FROZEN** to match the live TypeScript mailbox path (`createPermissionRequestMessage` /
  `createPermissionResponseMessage`, `sendPermissionRequestViaMailbox` /
  `sendPermissionResponseViaMailbox`). The older TS **directory** protocol
  (`~/.loom/teams/<t>/permissions/{pending,resolved}`) is **deliberately not ported**: request payloads
  live **only** as the `"text"` envelope of messages in the existing mailbox tree
  (`$LOOM_TEAM_RUNTIME_DIR/<sanitized team>/inboxes/<agent>.json`). Writing to the old directory
  protocol will not be seen by anything.

- **`cpp_migration/src/utils/swarm_helpers.cppm:529-532`** — `sandbox_permission_request`/`response` and
  `plan_approval_request`/`response` variants are **deferred** — no C++ worker-side sandbox network
  broker or plan gate emits them yet. When added they must use the **same inbox envelope**.

- **`cpp_migration/src/utils/swarm_helpers.cppm:777-782`** — `remove_mailbox_message_by_text` rewrites
  an inbox with one **exact-text** message removed, mirroring the TS legacy `removeWorkerResponse`
  semantics: **the blocking waiter deletes the response it consumed so a slow later poll can never
  redeliver it**. The envelope serializer is **replicated locally** because `team_helpers`'
  `detail::write_messages` is **not exported** from that module — so there are **two copies** of the
  envelope format that must agree.

- **`cpp_migration/src/utils/swarm_helpers.cppm:788-795`** — The inbox read-modify-write is protected
  **twice**: an in-process `std::mutex` shared with `write_to_mailbox` / `mark_all_read`, **and** a
  cross-process `flock`. Both are required — the mutex does not serialise other pane processes.

- **`cpp_migration/src/utils/swarm_helpers.cppm:720-721`** — `json_quote` duplicates `team_helpers`'
  `detail::mailbox_json_escape` **deliberately**, "kept local so this protocol module does not couple
  to that detail namespace". Two implementations, one format.

- **`cpp_migration/src/utils/swarm_helpers.cppm:830-843`** — **Worker-side "Always allow" grant
  persistence.** When the leader resolves with `AlwaysAllow`, the success envelope carries an SDK
  `PermissionUpdate` (`addRules` / `behavior: "allow"`); the worker **persists those rules and
  auto-allows matching future calls itself** — a background pane must not re-prompt the leader for the
  same tool on every turn. The grant store lives in the **shared team runtime dir** so it survives pane
  process restarts/reconnection. **Conservative enforcement:** only **whole-tool** grants (no
  `ruleContent`) auto-allow; content-scoped rules like `Bash(npm install)` are persisted but **NOT**
  matched here — the worker keeps asking the leader rather than risk a half-implemented command matcher
  granting too much.

- **`cpp_migration/src/utils/swarm_helpers.cppm:941-943`** — Every `addRules`/`allow` update is
  persisted in a **verbatim `permission_updates` JSON array**. Unknown/non-allow update shapes are
  **ignored** — the worker has no settings destinations other than its own grant file.

- **`cpp_migration/src/utils/swarm_helpers.cppm:852-854`** — The `destination` for the built update is
  `"session"`: pane workers are ephemeral and the grant store below is the real persistence — **the
  worker never touches settings files**.

- **`cpp_migration/src/utils/swarm_helpers.cppm:546-557`** — `permission_updates_json` is a
  **verbatim JSON array of SDK `PermissionUpdate` objects**: the leader sends an `addRules` update when
  the user chooses "Always allow".

- **`cpp_migration/src/utils/swarm_helpers.cppm:567`** — Request IDs have the shape
  `"perm-<unixms>-<7 base36 chars>"` (per TS `generateRequestId`). Anything parsing these must not
  assume a different format.

- **`cpp_migration/src/utils/swarm_helpers.cppm:570-572`** — `build_request_text` embeds the input
  object **verbatim** (brace-checked, with a `{}` fallback); the engine already produced it as parsed
  tool_use JSON.

- **`cpp_migration/src/utils/swarm_helpers.cppm:576`** — `build_response_text` shape: success →
  `{"response":{}}`, error → `{"error":...}`.

- **`cpp_migration/src/utils/swarm_helpers.cppm:837`** — A background pane **must not** re-prompt the
  leader for the same tool on every turn (the reason the grant store exists).

- **`cpp_migration/src/utils/swarm_backends.cppm:217-222`** — `capture_pane` returns `nullopt` when the
  pane does not exist or the backend cannot capture. The method is **non-pure (`= 0` is not used)** so
  test doubles in other translation units do not have to gain an override; the default means **"cannot
  capture"**.

- **`cpp_migration/src/utils/swarm_backends.cppm:486-501`** — Four iTerm2 operations are **explicit
  no-ops**: setting a pane color and setting a pane title both require **slow Python API calls**; pane
  titles are shown in tabs automatically; and pane balancing is handled automatically.

- **`cpp_migration/src/utils/swarm_backends.cppm:923`** — Hidden panes are tracked under a specific
  hidden session name.

- **`cpp_migration/src/utils/team_helpers.cppm:449-458`** — The mailbox JSON escaper follows
  **RFC 8259**: all other C0 control bytes must be `\u`-escaped, because **one bad control char must
  never corrupt the whole inbox**.

- **`cpp_migration/src/utils/team_helpers.cppm:96-104`** — `ScopedInboxLock::locked()` returns `true`
  on Windows where `flock` does not exist — a **no-op stand-in** where only the in-process mutex
  applies. Callers relying on cross-process safety get weaker guarantees on that platform.

- **`cpp_migration/src/utils/swarm_pane_observer.cppm:33`** — The poller `jthread` is allowed to do
  background `tmux` capture, but **FTXUI must not** be touched from it.

- **`cpp_migration/src/utils/swarm_pane_observer.cppm:58`** — `notify()` is invoked **unlocked** so a
  callback that **re-subscribes cannot** deadlock.

- **`cpp_migration/src/utils/swarm_pane_observer.cppm:42`** — A per-callback **alive guard** ensures
  `notify()` never invokes a callback whose owner has been destroyed.

- **`cpp_migration/src/main.cpp:608-612`** — **WORKER permission forwarding.** A pane/in-process
  teammate (identity set via `--agent-name` + `--team-name`; **the leader itself never has an agent
  name**) **must not pop its own local dialog in a background pane**. It asks the team leader over the
  mailbox and **blocks for the verdict, failing closed on timeout**.

- **`cpp_migration/src/ui/app_team_projection.cpp:464-467`** — The discriminator substrings come from
  the **frozen stage-A protocol**: the generic `"loom:permission"` tag grep is **intentionally NOT
  used** so that **only real envelopes parse**.

- **`cpp_migration/src/ui/app_team_projection.cpp:212-214`** — The implicit `"team-lead"` row is
  **not a teammate** and is filtered out; showing it duplicates the leader in the roster.

- **`cpp_migration/src/ui/app_team_projection.cpp:169-171`** — The observer callback runs on the
  **observer jthread** and **must not touch `screen_state_` off-thread**: it sets a flag and posts;
  identical to the statusline worker pattern. This is the required shape for any new subscriber.


- **`cpp_migration/src/utils/swarm_helpers.cppm:1089`** — The generic C++ dialog never edits input, so
  this path is normally a no-op.

### B.6 — Session storage / config / migrations

- **`cpp_migration/src/constants/paths.cppm:1-16`** — Two cascades live here, and they are **the ONLY
  place either is spelled out**. Before this module the same lookup was reimplemented in **eight
  walkers** (`query_engine`, `hooks/context`, `memdir/memory`, `memdir/paths`,
  `utils/system_directories`, `config/settings`, `hooks/shell_hooks`, and the memory commands), **each
  with its own hardcoded filename**. A change to any of them **silently applied to one code path and
  not the others**; keeping the order in one exported constant is what makes that impossible. The
  cascade is: `$LOOM_CONFIG_DIR` (explicit override, wins outright) → `$HOME/.loom` (current name) →
  `$HOME/.agents` (interop with the AGENTS.md ecosystem) → `$HOME/.claude` (the pre-rename name, so
  existing data keeps working).

- **`cpp_migration/src/constants/paths.cppm:104-112`** — The **write** config directory is
  **deliberately narrower than the read cascade**: `$LOOM_CONFIG_DIR`, else `~/.loom`. Reading a legacy
  `~/.claude` is safe and is the point of the cascade; **writing there is not the same act**. If a user
  has only `~/.claude`, writing our `sessions/`, `plugins/` and `settings.json` into it would
  **interleave our state with another tool's, in a directory the user did not choose for us** — and
  `~/.claude` already has its own `sessions/` for the other tool to collide with. So we create our own
  directory and leave theirs alone.

- **`cpp_migration/src/config/settings.cppm:200-204`** — The **user** scope resolves through the shared
  read cascade, so a user's existing settings are found after the rename. **Project** scopes use the
  project's own cascade directory name — **reading a legacy `~/.claude` must not make new projects
  write into `./.claude/`**.

- **`cpp_migration/src/memdir/paths.cppm:161-165`** — `loom_config_home()` uses the **WRITE** resolution
  **on purpose**: these are directories we create and manage (auto-memory, session-memory, `projects/`),
  so they belong under our own name even when a legacy `~/.claude` exists and is readable. Reading is a
  separate question.

- **`cpp_migration/src/memdir/paths.cppm:208-216`** — Session memory lives at
  `<config_home>/projects/<sanitized-cwd>/<sessionId>/session-memory/summary.md`. Unlike the long-term
  auto-memory, this file is **scoped to one session** and **accumulates the summaries produced at each
  compaction** so resumed/continuation runs **do not start blind after old messages are dropped**.

- **`cpp_migration/src/hooks/shell_hooks.cppm:546-551`** — User-scope hooks resolve through the
  **READ** cascade so a user's pre-rename `~/.claude/settings.json` hooks still load. The stated reason:
  **hooks are the surface where silently dropping them changes behaviour the user explicitly
  configured**, so this one must follow the cascade rather than only look at `~/.loom`.

- **`cpp_migration/src/state/persistence.cppm:100-105`** — The persisted state is a **flat,
  forward/backward-compatible object**: missing keys on read fall back to defaults, extra keys are
  ignored. It covers only the **user-preferences** class of `AppState` fields. **Runtime/transient
  flags (`is_loading`, `is_streaming`, `error_message`, `pending_*`) and conversation history are
  deliberately not persisted.** Adding a transient field to the persisted set re-introduces stale-state
  bugs on resume.

- **`cpp_migration/src/state/persistence.cppm:245-255`** — The state file is written via a **raw POSIX
  fd** so it can be `fsync`'d: `open(..., 0600)` → write → `fsync` → `rename`. An `ofstream` alone only
  flushes userspace buffers and is **not durable across a crash that happens between flush and rename**.

- **`cpp_migration/src/utils/file_persistence.cppm:122-124`** — `rename()` within a single filesystem
  is atomic at the syscall level: readers see either the old file or the new one, **never a
  half-written one**. This is why the temp-file + rename dance works and why the temp file must be in
  the **same directory**.

- **`cpp_migration/src/migrations/schema_versions.cppm:88-92`** — On startup, recovery from a torn
  write is specified exactly: if the primary exists and is parseable → use it **and drop any stale
  backup**; if the primary is missing/broken but the backup exists and is valid → **restore backup to
  primary** and return the backup's version; otherwise → return `0` (fresh install).

- **`cpp_migration/src/migrations/schema_versions.cppm:132-143`** — The atomic write guarantees, in
  order: (1) the parent directory exists; (2) an **advisory cross-process lock** is acquired so
  concurrent callers do not interleave the backup/temp/rename dance; (3) the new payload is written to
  a **random temp file in the same directory**, `fsync`'d, and then rotated — and critically, the
  existing primary is copied into the `.bak` slot **only after the temp is already on disk**, so **a
  torn rename cannot destroy the only good copy**.

- **`cpp_migration/src/migrations/migration_runner.cppm:114-118`** — Registering a duplicate migration
  version is an **error**, so callers fail fast on accidentally duplicated migration numbers **rather
  than silently overriding**. Silent override would make migration order non-deterministic.

- **`cpp_migration/src/migrations/migration_runner.cppm:226`** — A stricter user-provided
  `target_version` must **not** be overridden.

- **`cpp_migration/src/migrations/migration_runner.cppm:179`** — When `defer_commit` is true, **do not**
  invoke `on_version_changed` or `postflight_up`.

- **`cpp_migration/src/migrations/migration_runner.cppm:235-247`** — `run_atomically` treats pending
  migrations as a **single atomic unit** — either ALL of them apply or none — and internally sets
  `options.defer_commit = true`.

- **`cpp_migration/src/migrations/migration_runner.cppm:318`** — Postflight is
  **best-effort-once-durable: do not roll** back a successful durable commit because postflight failed.

- **`cpp_migration/src/migrations/migration_registry.cppm:32`** — Ensuring `schema_version` **never
  fails**.

- **`cpp_migration/src/migrations/migration_registry.cppm:42-53`** — Schema v1 is **deliberately
  minimal**: `sessions: { currentId: string|null, byId: {} }`. Rollback removes both fields.

- **`cpp_migration/src/migrations/migration_registry.cppm:20`** — TU-local entities are kept local to
  avoid exposing them across module boundaries (keeps Clang happy).

- **`cpp_migration/src/migrations/config_orchestrator.cppm:5-8`** — The orchestrator's two obligations:
  (a) write every dirty config JSON file atomically, then (b) acquire a **cross-process lock** so
  parallel invocations cannot race.

- **`cpp_migration/src/migrations/config_orchestrator.cppm:556-563`** — **NOTE: the orchestrator takes
  EXPLICIT responsibility for committing Pass B.** No `on_version_changed` hook is installed here;
  `run_atomically()` will invoke it with the final version **AFTER all config files have been
  written**. To keep the commit protocol (**files → version**) inside the closure, the installed
  `on_version_changed` performs a full two-phase commit of the dirty files **first**, then bumps
  `schema_version`. Inverting that order means the version file claims a state the files do not have.

- **`cpp_migration/src/migrations/config_orchestrator.cppm:526-534`** — In dry-run mode everything is
  still executed **in memory** with `defer_commit = true` and `auto_rollback = false`, with
  `on_version_changed` **not** called — the point is to report which files the suite **WOULD** mutate
  even when the user asked for zero writes.

- **`cpp_migration/src/migrations/config_orchestrator.cppm:174`** — `JsonMutDoc` is move-only, so
  `ConfigBundle` is move-only as well.

- **`cpp_migration/src/migrations/config_orchestrator.cppm:219`** — `bool` is checked **before**
  integral because `std::is_integral_v<bool>` is true — checking in the other order makes every boolean
  setting take the integer path.

- **`cpp_migration/src/migrations/config_orchestrator.cppm:260`** — Existing strings are read out of
  the current array **fully BEFORE any** mutation, because mutating while iterating invalidates.

- **`cpp_migration/src/config/settings.cppm:103-105`** — Keys the C++ port does **not** yet apply
  (permissions merge, hooks, `mcpServers`, managed/MDM settings, …) are recorded in `deferred_keys`
  **for honest feedback rather than silently dropped**. A new unimplemented key must be added to that
  list, not ignored.

- **`cpp_migration/src/config/settings.cppm:170-171`** — Recognition-but-not-application and outright
  non-understanding are both recorded, so the user learns their config had no effect.

- **`cpp_migration/src/utils/settings_manager.cppm:522`** — Existing settings are re-read from disk
  with the **cache bypassed**, to avoid merging against a stale cached copy.

- **`cpp_migration/src/utils/lockfile.cppm:135`** and **`utils/native_installer.cppm:228`** — A "stale"
  lock is one older than a max age, or one whose owning process is no longer running. Breaking a lock
  whose process **is** still running is the failure mode.

- **`cpp_migration/src/utils/cron_tasks.cppm:44`** — Stale lock cleanup runs **every hour**.

- **`cpp_migration/src/utils/graceful_shutdown.cppm:47`** — Calling complex functions from a signal
  context is **technically not safe**; this handler is deliberately minimal.

- **`cpp_migration/src/utils/graceful_shutdown.cppm:77`** — `std::function` does not support
  `operator==`, so shutdown handlers are unregistered by **priority matching** — registering two
  handlers with the same priority makes removal ambiguous.

### B.7 — Cross-module action-ordinal coupling

- **`cpp_migration/src/commands/compact.cppm:29-36`** — `ACTION_ADD_NOTIFICATION = 18` is the **ordinal
  of `ActionType::AddNotification` in `cc::state::ActionType`** and **must be kept in sync with
  `store.cppm`'s enum ordering**. Inserting a new enumerator anywhere but the end silently re-points
  every ordinal constant.

- **`cpp_migration/src/commands/brief.cppm:24-26`** — `ACTION_SET_BRIEF_ONLY = 24`, same rule.

- **`cpp_migration/src/commands/clear.cppm:29-32`** — Action-type ordinals must be kept in sync with
  `cc::state::ActionType` in `store.cppm`.

- **`cpp_migration/src/commands/cost.cppm:29`** and **`commands/plan.cppm:29`** — Ordinals of
  `ActionType::UpdateUsage` / `ActionType::SetPermissionMode`; same sync rule.

- **`cpp_migration/src/state/store.cppm:909-917`** — `EnableTool`, `DisableTool`, `SaveState`,
  `LoadState`, `ClearSavedState` are **deliberately no-ops in the reducer** until those effects have
  explicit payload and service semantics: they require behaviour outside pure `AppState`. Adding logic
  here would put side effects in a pure reducer.

- **`cpp_migration/src/state/store.cppm:944-956`** — The persistence **middleware is an intentional
  no-op**, and the reason is recorded: the TS store **does not implement a middleware pipeline at all**
  — its `Store` is a trivial `getState`/`setState`/`subscribe` object, and persistence of the few
  `AppState` fields the TS app persists happens through targeted **global-config writes** in
  `onChangeAppState.ts`, **not** through a store middleware. In the C++ port persistence is handled
  directly by the `Store` in `notify()`. The middleware exists **only for API parity** — do not move
  persistence into it.

- **`cpp_migration/src/state/store.cppm:987`** — The state-typed observer **decouples the subscriber**
  from the store instance.

- **`cpp_migration/src/commands/fast.cppm:82`** — The fallback must be kept **in sync** so non-AppState
  callers still see the right value.

- **`cpp_migration/src/constants/tools_constants.cppm:45-54`** — Tools disallowed for **ALL** agents
  (prevents recursion and mode conflicts). **AgentTool is conditionally disallowed based on user type in
  TS**, so in C++ that must be **configured at runtime** — it is deliberately **absent** from this
  static set.

- **`cpp_migration/src/skills/load_skills_dir.cppm:2288-2298`** — The file-access hook is registered so
  that file tools (`cc_tools`) can trigger skill discovery **without depending on `cc_skills`**
  (avoiding a circular dependency). Registration happens once at static-init or app startup.

- **`cpp_migration/src/skills/skill.cppm:380-384`** — See the same seam from the skills side.

- **`cpp_migration/src/services/memory/extract_memories.cppm:449-462`** — Memory extraction is invoked
  **through a runner callback so this service does not depend on the query engine**. It appears in the
  transcript as frontmatter `.md` files under the memory dir plus a one-line `MEMORY.md` index —
  exactly the TS `buildExtractAutoOnlyPrompt` contract.

- **`cpp_migration/src/services/plugins/installation_manager.cppm:1-17`** — This module has **ZERO
  live importers**, and there is no trivial single-call delegation (the real manager holds richer
  state). The body is **kept compiling but is intentionally not wired to a transport**. The real
  lifecycle backends are `cc.utils.plugin_manager`, `cc.utils.plugin_marketplace_lifecycle`,
  `cc.utils.plugin_marketplace`, and `commands/plugin/plugin_manage` — **prefer those**.

- **`cpp_migration/src/services/plugins/cli_commands.cppm:71-80`** — This shim is **superseded**; it
  **intentionally does NOT fake a success string** and instead returns an error naming the real entry
  point.

- **`cpp_migration/src/utils/plugin_manager.cppm:274-279`** — The install/uninstall/update signatures
  mirror the TS installed-plugins manager, but the **bodies are intentionally stubs**.

- **`cpp_migration/src/utils/plugin_marketplace.cppm:241`** — Marketplace **writes** are
  **intentionally out of scope** for the C++ CLI's headless install path.

- **`cpp_migration/src/utils/plugin_marketplace.cppm:702`** — On fetch failure the half-written config
  entry is **rolled back** (TS leaves it).

- **`cpp_migration/src/utils/plugin_validation.cppm:220`** — Unsupported plugin constraints are
  reported as an **error rather than a fake success**, **so callers cannot be silently misled**.

### B.8 — Identifiers, tokens, formats

- **`cpp_migration/src/ui/messages/tool_use_message.cppm:168-171`** — `parse_tool_status` accepts the
  shared projection contract strings `"pending" | "running" | "success" | "error"`, **tolerates
  `"cancelled"`**, and treats unknown/empty as `Pending`. `app.cppm` populates `entry.tool_status`; the
  renderer reads it through this helper. Changing the producer's vocabulary without changing this
  consumer (or vice versa) silently downgrades every unrecognized status to "Pending".

- **`cpp_migration/src/utils/tool_deny_rules.cppm:63-71`** — **Known TS limitation
  (`mcpStringUtils.ts`), replicate, do not "fix":** a server name containing `"__"` parses
  **incorrectly**. The C++ port is instructed to **reproduce** the misparse, because "fixing" it would
  change which rules match relative to every existing persisted rule.

- **`cpp_migration/src/utils/tool_deny_rules.cppm:30-39`** — Names are normalized for the API pattern
  `^[a-zA-Z0-9_-]{1,64}$`, with every char outside `[A-Za-z0-9_-]` becoming `_`. The TS original
  special-cased names carrying the **vendor's hosted-connector prefix**, collapsing underscore runs for
  those; **that branch is deleted**, with the reason recorded: there is no hosted connector service, so
  nothing can produce a name with that prefix, and the branch was **unreachable for every real MCP
  server name** (which comes from user configuration). The rationale is worth preserving: *"A prefix
  test that can never be true is not a safety net, it is a place for a future reader to believe
  something is handled that is not."*

- **`cpp_migration/src/utils/tool_deny_rules.cppm:87-100`** — Parsing `mcp__serverName__toolName`:
  splits on `"__"`, requires the first segment to be exactly `"mcp"` and a non-empty server segment,
  and **rejoins all segments after the server with `"__"` (so double underscores inside tool names are
  preserved)**. A bare `mcp__serverName` yields a `nullopt` tool.

- **`cpp_migration/src/utils/tool_deny_rules.cppm:191-204`** — Permission-rule grammar: a closing paren
  **must end the string**; a missing tool name is malformed; `''` or `'*'` means **tool-wide**.

- **`cpp_migration/src/utils/tool_deny_rules.cppm:263`** — `ruleContent` **defined** ⇒ **no
  whole-tool match**: a content-scoped rule does not deny every invocation of the tool.

- **`cpp_migration/src/utils/tool_deny_rules.cppm:173`** — **Only the presence of content matters** for
  deny filtering; content-bearing rules are evaluated by content, not by tool name.

- **`cpp_migration/src/utils/tool_deny_rules.cppm:119`** — Legacy tool-name renames are applied so old
  rules resolve to canonical names — removing the alias table orphans persisted rules.

- **`cpp_migration/src/utils/tool_deny_rules.cppm:280`** — An empty rule list matches **nothing**;
  garbage rules simply match nothing (fail-open on parse, not fail-closed).

- **`cpp_migration/src/tools/runtime_shared_utils.cppm:126`** — A specific human-readable prefix is
  used when a pending user-visible message is queued; consumers match on it.

- **`cpp_migration/src/ui/messages/message_row.cppm:571`** — Core message rendering lives in
  `error_message.cppm` (the FTXUI upgrade); `message_row.cppm` dispatches rather than renders.

- **`cpp_migration/src/ui/messages/messages_interactions.cppm:55-58`** — `MessageMetadata` is defined
  **here**, not in `messages_list`, so the interaction primitives have a **stable ABI to code against
  today**. The intent (recorded) is that UI21's `messages_list` should later expose a
  `MessageRowPayload` that **contains** a `MessageMetadata`; until then callers **copy** the relevant
  fields into the view-model. Two sources of truth exist in the interim.

- **`cpp_migration/src/ui/messages/messages_interactions.cppm:101-105`** — Same intent restated: UI21's
  `MessageRowPayload` **should, in a follow-up commit, embed this struct**.

- **`cpp_migration/src/ui/messages/messages_interactions.cppm:695`** — When consumed, the caller
  **must not propagate** the event into the message list.

- **`cpp_migration/src/ui/messages/messages_interactions.cppm:1040`** — Pixel-perfect alignment is
  **not enforced**; in a TTY only the stated guarantee holds.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:78-85`** — `VirtualList` **intentionally
  keeps its own `VisibleRow` struct**. `cc.ui.messages.messages_list` is a separate, larger module that
  imports 25+ per-row type modules; importing it here would cause a **cascade of BMI size issues and
  potential circular edges**. Instead the *caller* knows both types and builds a
  `vector<VisibleRow>` snapshot via a small conversion function — the same decoupling as TS.
  "Consolidating" the two row types would re-create the BMI cascade.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:877-882`** — The new-row count is capped at
  the row-count delta because **reordering or filter changes can make the estimate double-count**.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:874`** — A safety cap of 10000 iterations
  bounds the counting loop.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:603-607`** — The occurrence placeholder
  uses `prefixSum[ptr] + 1` (first occurrence in this message) for simplicity, whereas TS uses
  `prefixSum[ptr+1]` when the delta is negative — a **documented simplification**, so the badge number
  can differ from TS when stepping backwards.

- **`cpp_migration/src/ui/messages/messages_list.cppm:545-556`** — The search-text cache vector
  **grows to match `rows.size()` on first access**; entries must not be discarded without shrinking it,
  or indexing goes out of range.

- **`cpp_migration/src/ui/messages/messages_list.cppm:1842-1844`** — `build_visible_rows` has
  **already FILTERED** the visible set by query. The virtual-list search engine exists for **NAVIGATION
  within that set** (finding which row contains the query, jumping to nearest match) — it is **not** a
  second filter. Using it to filter double-filters.

- **`cpp_migration/src/ui/messages/messages_list.cppm:750`** — The message-field extraction is a
  **lightweight scan, deliberately not a full JSON parser**, valid only for the known field names.

- **`cpp_migration/src/ui/messages/messages_list.cppm:966-967`** — Completed thinking is **hidden in
  transcript mode**, so it is **not indexed** for search — searching will not find it.

- **`cpp_migration/src/ui/messages/messages_list.cppm:107-108`** — `ftxui/component/input.hpp` is **not
  a standalone header** in upstream FTXUI; `Input()` is exported via
  `ftxui/component/component.hpp`. Including the former fails on some FTXUI builds.

- **`cpp_migration/src/ui/messages/messages_list.cppm:1113`** — `TranscriptCapDivider`'s count is the
  number of messages **hidden by the cap**.

- **`cpp_migration/src/ui/messages/messages_list.cppm:305`** and **`:508`**, **`:485`** — In brief mode,
  assistant text + thinking are hidden; only brief-tool calls + their results + real user input show.

- **`cpp_migration/src/ui/messages/messages_list.cppm:371`** — A **real** user message (non-tool_result)
  advances the turn counter; tool results do not.

- **`cpp_migration/src/ui/messages/messages_list.cppm:542`** — The unseen-divider anchor is a
  **24-character prefix match** on the message uuid (empty strings allowed).

- **`cpp_migration/src/ui/screens/repl_screen.cppm:1145`** — Same 24-char uuid prefix anchor rule at the
  REPL side.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:1520`** — `useUnseenDivider`'s `onScrollAway` fires
  only on the **FIRST** scroll-away from the bottom; firing on every scroll re-shows the divider
  constantly.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:3301-3305`** — After the click, `stickyPrompt` stays
  at the `'clicked'` sentinel **until** something that moves the viewport writes a new `sticky_prompt`.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:1011`** — The visible-row helper skips
  system/progress rows when counting the non-assistant→assistant transition.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:999`** — Tool-use entries **never** have visible text
  content to the user.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:941`** — The pulse anchor is re-sampled when the mode
  transitions from Hidden to something (frame==0).

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2000`** — `'!'` is **swallowed as a mode trigger** by
  the char handler, so typing it has no literal effect on the buffer.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2045`** — Non-empty input immediately flips the
  **effective** mode to Bash. Note the word *effective* — the stored mode may differ.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:4232-4234`** — `Ctrl+L` uses `CSI H` (`'\x1b[H'`) and
  repaints; see A.10 for the "must not mutate input" half.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:4258-4260`** — The history-search path reads persisted
  prompt history from `~/.loom/history.jsonl` and does **substring** matching (not prefix, not fuzzy).

- **`cpp_migration/src/ui/screens/repl_screen.cppm:4716`** — UTF-8 lead bytes are `0xC0..0xFF`;
  continuation bytes are `0x80..0xBF` — the input decoder depends on this split.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:4099-4104`** — The dialog-queue priority block is
  **intentionally empty**: `has_standalone`/`modal`/`overlay`/`bottom` are checked **inline inside
  `DispatchDialogQueueEvents()`** (called above), so the per-slot helpers are **not** re-exposed as free
  predicates here to avoid a duplicate definition with `dialog_queue_render`.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2784`** — This rendering mode is a
  **full-takeover** — it replaces the entire render, not a slot.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:268`** — The collapsed "Thinking" projection is a
  **static** projection, distinct from the interactive component.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:537`** — The comment string here is the **FALLBACK
  only**; the actual displayed placeholder comes from elsewhere.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:290`** — Parsed tool input JSON is threaded into the
  renderer through a specific options struct rather than being re-parsed per row.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:316`**, **`:322`**, **`:324`** — `maxRetries` is the
  total allowed attempts; `sessionExpired` suppresses the Retry pill and shows "Clear session" instead.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:400`**, **`:529`**, **`:531`** — Voice state is
  `'idle' | 'recording' | ...`; `pasted_contents_` maps paste-id → image, and **also** holds
  `text`-type entries for truncated text pastes (two different content kinds in one map).

- **`cpp_migration/src/ui/screens/repl_screen.cppm:759`** — The permission mode is cycled via
  Shift+Tab (with no suggestions).

- **`cpp_migration/src/ui/screens/doctor_screen.cppm:691-699`** — The permission-integrity check reports
  a **Warning** when a wildcard allow `"**"` and a `deny` both appear, because **a "deny" rule that
  appears after a "**" allow can never fire**. The check is a heuristic substring scan over the
  settings file (it matches `"**"` next to `allow`, and `deny` anywhere), not a parsed rule-order
  analysis.

- **`cpp_migration/src/ui/screens/resume_screen.cppm:1646-1647`** — Model cycling here is a
  **simplification**: `"" → first model we see → next`, and it may **prompt** instead.

- **`cpp_migration/src/ui/screens/log_selector.cppm:593-595`** — These helpers are declared
  out-of-line **to keep `refresh_view` readable**; the forward declarations exist to keep the compiler
  happy in a single-module world.

- **`cpp_migration/src/ui/screens/log_selector.cppm:68`** — This is the designated place for
  cross-module imports of tokens / UI8 trust primitives.

- **`cpp_migration/src/ui/autocomplete_sources.cppm:126-129`** — `replacement_start`/`end` are passed
  through **verbatim** — the caller knows cursor/token positions; this layer must not recompute them.

- **`cpp_migration/src/ui/autocomplete_sources.cppm:97-98`** — History suggestions are
  **project-filtered** and **newest-first**; an empty query means "show recent".

- **`cpp_migration/src/ui/autocomplete_sources_impl.cpp:349`** — Prompt history is stored at
  `{getConfigHomeDir()}/history.jsonl`.

- **`cpp_migration/src/ui/autocomplete_sources_impl.cpp:419-420`** — `readLinesReverse` reads in **4 KB**
  chunks — the reverse read is chunked, so a record straddling a chunk boundary must be handled.

- **`cpp_migration/src/ui/autocomplete_sources_impl.cpp:500`** — A **file lock** protects concurrent
  history writes.

- **`cpp_migration/src/ui/autocomplete_sources_impl.cpp:515`** — History entries are **deduped by
  display** and capped at `MAX_HISTORY_ITEMS` (100).

- **`cpp_migration/src/ui/autocomplete_sources_impl.cpp:263`** — Agent suggestions match on
  `agentType` **or** `displayText`.

- **`cpp_migration/src/ui/app_autocomplete.cpp:453-455`** — **SL-01**: if the user typed the **full
  name** of a hidden command, it is surfaced at the **top** of the suggestion list — the hidden-command
  escape hatch.

- **`cpp_migration/src/ui/app_autocomplete.cpp:1039-1041`** — Thinking entries are pruned after a
  **30-second grace period**, and the prune happens **before** the `has_in_flight` check **so stale
  entries don't keep the projection path alive**.

- **`cpp_migration/src/ui/app_autocomplete.cpp:1033-1037`** — If any streaming tool has
  `complete=true`, the turn's `AssistantMessage` was already committed, so **all** streaming blocks
  (text + thinking + tool-use) from that turn are **duplicates** of the committed path.

- **`cpp_migration/src/ui/app_autocomplete.cpp:1081`** — A text block always has index equal to the
  **highest** block index — relied on for ordering.

- **`cpp_migration/src/ui/app_autocomplete.cpp:283`** — `@history <query>` is a special
  with-space trigger form of `@history`.

- **`cpp_migration/src/ui/app_autocomplete.cpp:1241-1246`** — `Ctrl+V` is detected as
  `Event::Character('\x16')` — the same pattern used by `text_input.cppm:586`. Note also that
  `osascript` clipboard reads are **offloaded to a** worker so they do not block the render loop.

- **`cpp_migration/src/ui/app_autocomplete.cpp:907`** — The capture-pass callback **cannot** flag or
  post on a torn-down adapter — it must check liveness.

- **`cpp_migration/src/ui/app.cppm:422-426`** — **TS PARITY FIX (2026-07-05).** `is_tool_use` **must be
  false** for tool results. The `repl_screen` dispatcher checks `is_tool_use` **FIRST**, so a
  `tool_result` with `is_tool_use = true` gets routed to `AssistantToolUse` rendering instead of
  `UserToolResult` — **the committed result card never appears as a separate block**.

- **`cpp_migration/src/ui/app.cppm:455-459`** — Same fix, stated as an invariant:
  `ToolResultMessage` is the *committed result* (`role="tool"`), **not** the tool_use request
  (`role="assistant"`).

- **`cpp_migration/src/ui/app.cppm:462-465`** — The tool name **must** be propagated from the result
  message, otherwise the renderer shows generic `"tool"` and **every tool result looks anonymous**.

- **`cpp_migration/src/ui/app.cppm:654-662`** — **TS PARITY FIX (2026-07-04).** The API returns tool
  results as `role=user` messages with `tool_result` content blocks. The old code **ignored** these, so
  **committed tool results vanished from the transcript after streaming ended** (`streaming_tools_` was
  cleared but the committed `UserMessage` with `ToolResultBlock` was never projected).

- **`cpp_migration/src/ui/app.cppm:496-502`** — **`content_preview` IS the rendered message body, not a
  "preview". Do NOT truncate here** — `VirtualMessageList` handles display clipping. Truncating caused
  a **"text vanishes after streaming"** bug: the live stream showed the tail (last 500 chars) but the
  committed view showed only the head (first 500 chars), so for >500-char responses the content the
  user was reading **disappeared on completion**.

- **`cpp_migration/src/ui/app.cppm:542-553`** — The old projection grouped all text into one accumulator
  and all tools into a separate vector, emitting `[thinking, merged_text, tool1, tool2, …]`. For
  `[text1, tool_use, text2]` this produced `[text1+text2, tool_use]` — **text was merged and the order
  was wrong**, gluing the model's final response to the pre-tool announcement. **Consecutive text
  blocks ARE merged** (TS renders adjacent `<Text>` nodes inline), **but any non-text block (thinking,
  tool_use) flushes the text accumulator** and emits its own row.

- **`cpp_migration/src/ui/app.cppm:507-517`** — `project_messages` splits a single `AssistantMessage`
  into **MULTIPLE display rows** when it mixes a `ThinkingBlock` with a `TextBlock` / `ToolUseBlock`.
  The legacy single-entry projection **collapsed them into one thinking row, which hid the visible
  answer** once thinking rows were routed through the collapsed renderer.

- **`cpp_migration/src/ui/app.cppm:767-769`** — The local `!` bash command worker runs the command
  **outside the LLM turn** (`shouldQuery:false`), so it uses **its own thread** rather than
  `query_thread_` and **never sets `query_running_`**.

- **`cpp_migration/src/ui/app.cppm:866-871`** — Cross-thread handoff: the bash worker communicates via
  `pending_bash_result_` under `bash_result_mutex_` so that `local_command_messages_` / `SyncState` are
  **only ever mutated on the render thread**.

- **`cpp_migration/src/ui/app.cppm:873-876`** — The streaming-markdown stable-prefix cache is **reset
  alongside `streaming_text_`** so each new model response starts with a **fresh** stable prefix. Not
  resetting it makes the next response render against the previous response's prefix.

- **`cpp_migration/src/ui/app.cppm:1808`** — The collapse chain is run on the **raw conversation**,
  before the same transformation applied elsewhere.

- **`cpp_migration/src/ui/app_constructor.cpp:276-279`** — When spawned as a tmux/iTerm pane teammate,
  the filesystem inbox poller starts **only** under that identity; the leader-side counterpart polls the
  team-lead mailbox for stage-A messages.

- **`cpp_migration/src/ui/app_constructor.cpp:194`** — See A.10: input state must not be mutated on redraw.

- **`cpp_migration/src/ui/app_dialog_registration_teams.cpp:8`** — The registration borrows
  `ReplScreenState` from `DialogRenderContext` **for `Render()` only** — writing through it during
  registration is out of contract.

- **`cpp_migration/src/ui/app_team_projection.cpp:15`** — Kept in its own TU (mirroring
  `app_agent_menu.cpp` / `app_extra_methods.cpp`) to keep module size down.

- **`cpp_migration/src/ui/plugins/plugin_install_flow.cppm:644`** — If the caller supplied a
  `PluginDefinition` via inputs, it is consumed here; the pure-UI contract keeps this path.

- **`cpp_migration/src/ui/plugins/plugin_install_flow.cppm:726`** — `wizard_state.current_step_index`
  **cannot be read directly** — the flow must go through the accessor.

- **`cpp_migration/src/ui/mcp/mcp_server_details.cppm:139-141`** — `status_badge_decor` is a
  **standalone re-declaration** of `status_badge` to **avoid cross-module inline ordering issues** —
  the duplicate is intentional and must be kept in sync by hand.

- **`cpp_migration/src/ui/mcp/mcp_server_details.cppm:945`** — Enter "edits" the focused setting, which
  for bools is a toggle and for strings is a **no-op**.

- **`cpp_migration/src/ui/mcp/mcp_add_server_wizard.cppm:792`** — Shake / no-op means the step **can't
  advance** (validation failed); it is not a crash.

- **`cpp_migration/src/ui/design_system/theme_provider.cppm:1-12`** — The `ThemeProvider` is a
  **lightweight analog** of the TS `ThemeContext`. It **intentionally does NOT carry the full 89-field
  TS `Theme` struct across the FFI**: it exposes the compact `Palette` + accessibility flags the design
  primitives actually consume. Full field access remains available via the legacy
  `ui/design/themed_text.cppm` module.

- **`cpp_migration/src/ui/design_system/theme_provider.cppm:344-346`** — **17 of the 69 TS `Theme`
  fields were not handled** by the color-name lookup and **silently fell through to `p.text`**. They are
  now mapped explicitly (`autoAccept`, `bashBorder`, `promptBorder`, `userMessageBackground`, …), each
  accepting both camelCase and snake_case spellings. Adding a new TS theme field without adding it here
  silently returns `p.text`.

- **`cpp_migration/src/ui/design_system/theme_provider.cppm:380`** — FTXUI has **no React-style context
  provider**; theming is applied by wrapping components instead.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:127`** — Field naming convention:
  **snake_case versions of the TS camelCase identifiers**, so the TS name is recoverable from the C++
  name.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:58-67`** — Animation easing constants and
  HSL sweep timings are **copied verbatim** from the TS animated-asterisk component; the animation
  phase is driven off these.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:58`** / **`:1198`** — **PHASE_5 NOTE:** the
  TS side drives all animation via `useAnimationFrame`; FTXUI v5 does **not** expose `Color::red()`
  accessors on the color type, so a manual clamp/parse path is used instead.

- **`cpp_migration/src/utils/parse_references.cppm:77-90`** — The reference pattern is
  `\[(Pasted text|Image|\.\.\.Truncated text) #(\d+)(?: \+\d+ lines)?(\.)*\]` with the `g` flag.
  Matches are returned in **increasing `index` order**, and entries with **`id <= 0` are dropped**.
  `\.\.\.` is kept escaped **character-for-character identical** to TS even though the dots are literal
  in that context.

- **`cpp_migration/src/utils/parse_references.cppm:85-90`** — `std::regex` runs in its default
  **ECMAScript** grammar, which supports exactly the features this pattern uses (alternation, capturing
  and non-capturing groups, `?`/`*`/`+`, `\d`, character-class escapes). Switching grammar breaks it.

- **`cpp_migration/src/utils/parse_references.cppm:110`** — The `id <= 0` filter, restated at the match
  site.

- **`cpp_migration/src/ui/prompt/prompt_paste_handler.cppm:210-218`** — Image-type sniffing thresholds:
  JPEG needs **only 3 bytes** (`FF D8 FF`) but the code guards with `size >= 4`; WebP requires
  `"RIFF"` at offset 0 **and** `"WEBP"` at offset 8.

- **`cpp_migration/src/utils/hyperlink.cppm:165-174`** — `path_to_file_url` reproduces Node's
  `url.pathToFileURL(p).href` **byte-for-byte**, including the **empirically-verified Node v22 safe
  set** `A-Za-z0-9 ! $ & ' ( ) * + , - . : ; = @ _`. Note the non-obvious members of that set: `'~'`
  **AND** `'[' ']'` **ARE** encoded (`%7E` / `%5B` / `%5D`); non-ASCII bytes are encoded as their raw
  UTF-8 bytes (`0xC3 0xA9` → `%C3%A9`); space → `%20`, `#` → `%23`, `?` → `%3F`, `%` → `%25`; and
  `&` stays **raw**. This was derived by testing, not by reading a spec.

- **`cpp_migration/src/utils/hyperlink.cppm:122`** — Malformed URLs return `std::nullopt` **instead of
  throwing** — the TS code catches and silently ignores `fileURLToPath` errors.

- **`cpp_migration/src/utils/hyperlink.cppm:141`** — Other authorities (e.g. `file://host/share`)
  produce **UNC paths**; for simplicity the path is kept as-is after stripping `"//"`. This is a known
  approximation.

- **`cpp_migration/src/utils/hyperlink.cppm:218-237`** — Only `http://` and `https://` URLs are opened
  in a browser; **any other scheme is silently ignored** (returns `false`), matching TS. A malformed
  `file:` URL would make `fileURLToPath` throw, so it is caught and ignored silently.

- **`cpp_migration/src/utils/hyperlink.cppm:302-313`** — **When FTXUI's `hyperlink(url)` decorator is
  available, prefer it** — it registers the URL with the `Screen` so **pixel-level click detection**
  works even when mouse tracking intercepts terminal-native OSC 8 clicks. This free function is for
  **non-FTXUI contexts** (e.g. direct stdout writes from tool output formatters).

- **`cpp_migration/src/utils/hyperlink.cppm:61`** — Browser launching respects the `$BROWSER`
  environment variable on non-Windows.

- **`cpp_migration/src/utils/http_encoding.cppm:67`** — On invalid percent-sequences the **malformed
  bytes are passed through** rather than rejected.

- **`cpp_migration/src/utils/osc_9_4.cppm:44`** — The progress operation codes correspond to the TS
  `PROGRESS` constants.

- **`cpp_migration/src/utils/clipboard.cppm:246-250`** — `osascript` writes clipboard PNG data to a temp
  file using a byte-sequence approach shared with `has_image()`; the same approach is used in the C++
  source, not a shell string.

- **`cpp_migration/src/utils/text_highlighting.cppm:89`** — Overlapping highlights are resolved by
  **keeping a highlight only if its range does not overlap** an already-kept one (first-wins order).

- **`cpp_migration/src/utils/text_highlighting.cppm:157`** — Highlights containing the cursor position
  are filtered so the character under the cursor is not double-styled.

- **`cpp_migration/src/utils/set_utils.cppm:70`** — Symmetric difference means items present in
  **exactly one** input set.

- **`cpp_migration/src/utils/file_edit_utils.cppm:1037`** — Two file-edit inputs are **never**
  equivalent if they target **different files**.

- **`cpp_migration/src/utils/file_edit_utils.cppm:194-202`** — The Unicode-letter check wraps
  `\p{L}` (TS `/\p{L}/u`) but **falls back to ASCII-only when the regex engine doesn't support it at
  runtime**. Consequence: non-ASCII contractions are not detected on such platforms — a documented
  degradation.

- **`cpp_migration/src/utils/file_edit_utils.cppm:1074-1078`** — Encoding / line-ending detection lives
  in a full file-read module; this helper **deliberately keeps the default path (UTF-8, LF) simple** so
  the caller can layer platform specifics on top.

- **`cpp_migration/src/utils/file_edit_utils.cppm:1101-1105`** — BOM sniffing only recognises
  `0xFF 0xFE` → UTF-16 LE; **unrecognised BOMs are treated as binary and fall back to UTF-8** (callers
  validate). TS decodes via `fileBuffer.toString(encoding)` and then `replaceAll("\r\n", "\n")`; the C++
  only approximates this.

- **`cpp_migration/src/utils/file_edit_utils.cppm:306-316`** — The structured patch uses a **Myers
  O(ND) LCS** line diff, and the code **deliberately avoids shelling out to `diff -u`** for two stated
  reasons: (a) callers rely on `PatchHunk` line data to build UI widgets, and (b) **subprocess calls are
  expensive when the model proposes many small edits per turn**. Callers wanting a shell `diff -u`
  display can use `bash_helpers` on the two files.

- **`cpp_migration/src/utils/file_edit_utils.cppm:186-190`** — The contraction heuristic is an
  apostrophe between two letters (e.g. `don't`, `it's`).

- **`cpp_migration/src/utils/skill_usage.cppm:21`** — The usage score is `usage_count * factor`.

- **`cpp_migration/src/utils/model/validate_model.cppm:14`** — A **simplified `ModelInfo`** is defined
  locally to **avoid a cross-module dependency** — it does not have the full model metadata.

- **`cpp_migration/src/utils/system_theme.cppm:27`** — The concrete theme name is **always resolved,
  never `'auto'`** — callers can rely on getting a real variant.

- **`cpp_migration/src/utils/cron_scheduler.cppm:17`** — `CronExpression` is **simplified to avoid
  cross-module deps at build time**; the `-1 = wildcard` sentinel is how absence is encoded.

- **`cpp_migration/src/utils/abort_controller.cppm:173`** — `create_never_cancelled_token()` exists for
  call sites that need to satisfy the token parameter without cancellation semantics.

- **`cpp_migration/src/services/remote_settings/security_check.cppm:11`** — A specific set of settings
  **cannot be overridden remotely**. Adding a setting to the remote-overridable set without adding it
  here weakens the security boundary.

- **`cpp_migration/src/services/remote_settings/sync_cache.cppm:33`** — "Stale" means older than the
  TTL.

### B.9 — Skill / agent prompt and content contracts

- **`cpp_migration/src/tools/built_in_agents.cppm:346`** — The frontend-change guidance is a
  **behavioural instruction to the agent**, not documentation: it forbids saying "needs a real browser"
  without attempting browser automation, and requires curl-ing a sample of page subresources because
  **HTML can serve 200 while everything it references fails**. Editing it changes agent behaviour.

- **`cpp_migration/src/tools/agent_memory.cppm:1-10`** — The low-level helpers (`agent_memory_dir`,
  `load_agent_memory_prompt`, `agent_memory_scope_note`, `sanitize_agent_memory_component`) live in
  `cc.tools.agent` (`agent_tool.cppm`), not here, so the rest of the codebase does not need to pull the
  entire Agent tool implementation. This module only re-exports the remaining public API.

- **`cpp_migration/src/tools/agent_memory_snapshot.cppm:76-79`** — The TS uses Zod schemas; the C++ uses
  an **intentionally loose string search** so a malformed file **simply returns `nullopt` instead of
  crashing**.

- **`cpp_migration/src/tools/agent_type_resolution.cppm:37-39`** — This is a **lightweight alias** for
  `agent_runtime`'s `resolve_requested_agent_type`; it returns the **canonical** `agent_type` string as
  it appears in the `AgentDefinition` list, **or `nullopt` when no unambiguous match exists**. Two entry
  points, one implementation — do not fork the logic.

- **`cpp_migration/src/tools/agent_tool.cppm:220-221`** — `TODO(agent-split)`: placement of these
  `using`-imports is **ambiguous**; they remain in the `agent_tool` root via using-imports from
  submodules. Reorganizing without resolving the split breaks the module.

- **`cpp_migration/src/ui/agents/agent_wizard.cppm:67-73`** — `cc.ui.wizard_dialog` exposes
  `MakeWizard(WizardConfig, StepsFn)` whose steps carry `Element`-render + event callbacks; the agent
  wizard uses a **different pattern** (props object + steps that return full `Component` objects). Local
  adapter types bridge the two; the imported `WizardStep` type is **not** used directly.

- **`cpp_migration/src/ui/agents/agent_details_dialog.cppm:27`**, **`:148`**, **`:735`**, **`:853`** —
  The delete button **delegates to a `TrustDialog`**, which the caller wires; `"D"` calls `on_delete` and
  the caller **must** wrap it in a `TrustDialog{severity=Critical}` confirmation. The dialog itself does
  **not** confirm.

- **`cpp_migration/src/ui/agents/agent_cards.cppm:97`** — The last-run timestamp is displayed
  **verbatim** (ISO-ish), not reformatted.

- **`cpp_migration/src/skills/bundled/debug.cppm:183`** and **`skills/skill.cppm:310`**,
  **`skills/bundled/verify.cppm:276`** — Invalid patterns are **skipped silently**; a bad regex in a
  skill definition must not abort the whole scan.

- **`cpp_migration/src/skills/bundled/debug.cppm:567`** — Actual edit application **depends on harness
  permission** — this code path proposes, it does not apply.

- **`cpp_migration/src/skills/bundled/loop.cppm:278`** and **`skills/bundled.cppm:99`** — The loop
  skill's stop conditions come in two flavours: **explicit** (`regex` / keywords) and **implicit**.

- **`cpp_migration/src/skills/bundled/loop.cppm:134`** — Output is normalized **for implicit-stable
  comparison** — the normalizer's exact output is the comparison key.

- **`cpp_migration/src/skills/bundled/update_config.cppm:334`** — Only a **hook** in `settings.json`
  can trigger automation; **memory/preferences cannot**.

- **`cpp_migration/src/skills/bundled/stuck.cppm:15`** — The diagnostic **respects a user-provided
  PID/symptom** rather than guessing.

- **`cpp_migration/src/skills/bundled.cppm:140-142`** — `run_loop()` delegates LLM calls to the
  **caller** via a config — the skill module does not hold a query engine reference.

- **`cpp_migration/src/services/prompt_suggestion/prompt_suggestion.cppm:16-19`** — A **NEW
  deterministic heuristic ranker** replaces the previously-hardcoded single suggestion. It is
  **documented as a placeholder heuristic** until the LLM fork path is ported, and the file explicitly
  says **do NOT claim parity with the TS LLM path**.

- **`cpp_migration/src/services/prompt_suggestion/prompt_suggestion.cppm:394-401`** — The suppress-reason
  gates map onto C++ `AppState` fields where they exist. The TS engine additionally checks
  `appState.elicitation.queue.length > 0` and `currentLimits.status !== 'allowed'`; the C++ has **no
  elicitation queue or rate-limit service**, so those two reasons are **intentionally NOT produced**.
  This is an **explicit, documented gap** — the function returns `nullopt` for them.

- **`cpp_migration/src/services/prompt_suggestion/prompt_suggestion.cppm:444-452`** — The ranker is a
  **CONSERVATIVE PLACEHOLDER**: it **cannot match** the TS LLM suggestion quality and **does not claim
  parity**. It is deterministic, offline, and **replaceable later** by the LLM path.

- **`cpp_migration/src/services/prompt_suggestion/prompt_suggestion.cppm:679-683`** — A tiny read-only
  bash classifier replaces the full TS validator; it is **intentionally conservative** because
  **false-stops are safe (speculation just aborts)** while false-allows are not.

- **`cpp_migration/src/services/prompt_suggestion/prompt_suggestion.cppm:656-657`** — `'complete'` is
  included in the `CompletionBoundary` variant for completeness but is **only ever produced by the agent
  loop, never by the pure classifier**.

- **`cpp_migration/src/services/prompt_suggestion/prompt_suggestion.cppm:127-129`** — The fixed
  suggestion prompt is **ported verbatim** and documents the intent model, but is **NOT used by the
  deterministic ranker** — its only purpose is to be ready when an LLM fork path exists.

- **`cpp_migration/src/services/prompt_suggestion/prompt_suggestion.cppm:223-261`** — Several regexes
  are hand-replicated (`\bsilence is\b`, `\bstay(s|ing)? silent\b`, a prefixed-label pattern like
  `Note: foo`); they are case-insensitive and byte-for-byte equivalent to the TS originals.

### B.10 — Miscellaneous invariants

- **`cpp_migration/src/commands/security_review.cppm:160-164`** —
  **IMPORTANT: the regex list lives in `secret_scanner.cppm`. We do NOT copy it here** — the scan is
  **delegated**. A second secret-pattern list would drift from the first.


- **`cpp_migration/src/services/team_memory/secret_scanner.cppm:32-33`** — A detected secret exposes
  **only the gitleaks rule id and a human-readable label**; **the matched text is intentionally NOT
  returned**. This is a deliberate non-leak: returning the match would put the secret itself into logs
  and messages.


- **`cpp_migration/src/services/team_memory/secret_scanner.cppm:48`** — The API-key prefix is
  **assembled at runtime so the literal byte sequence** never appears in the binary — required for
  secret scanners that scan the source itself.

- **`cpp_migration/src/constants/cyber_risk.cppm:3`** — **IMPORTANT: DO NOT MODIFY THIS INSTRUCTION
  WITHOUT SAFEGUARDS TEAM REVIEW.** The safety instructions in this file are gated on an external
  review; local edits are out of process.

- **`cpp_migration/src/commands/command_registry.cppm:134-141`** — The `help` handler with no args
  **must return the metadata string `"UI:help"`** so the app shell opens the `HelpView` dialog. Returning
  ordinary text instead produces a text dump where the dialog was expected.

- **`cpp_migration/src/commands/command.cppm:367-370`** — **SL-01:** `hidden_command_if_exact` returns a
  hidden command only on an **exact name match**, else `nullptr`. TS lets users reach hidden commands by
  typing their **full name**; because `visible_commands()` hides them entirely, **this is the only escape
  hatch**. Removing it makes hidden commands unreachable.

- **`cpp_migration/src/commands/command_registry.cppm:219-227`** — Same escape hatch, delegating to the
  core; plus `SL-03`: lookup of a **VISIBLE** command definition by exact name for argument completion
  (the two lookups deliberately have different visibility rules).

- **`cpp_migration/src/commands/color.cppm:76-82`** — **Teammate guard:** a session that is a swarm
  teammate **cannot set its own color** — teammate colors are assigned by the team leader. The error
  message is user-visible and states the reason.

- **`cpp_migration/src/commands/plugin/plugin_ui_data.cppm:225-228`** — The plugin help text is a
  **byte-for-byte match of the TS `PluginSettings.tsx` help JSX**, and note: it uses **plain hyphens
  "-", not em-dashes**. It is kept as a single constant so the UI layer displays it verbatim — editing
  it (or "tidying" the hyphens) breaks the match.

- **`cpp_migration/src/commands/plugin/plugin_ui_data.cppm:297-305`** — The real implementation consults
  `getSettingsForSource`; this function is **the data-flow contract** called *after* consulting those
  layers. Editable sources are checked **in order: user, project, local**.

- **`cpp_migration/src/commands/plugin/plugin_ui_data.cppm:499`** — The trust-warning data contract is
  derived from `PluginTrustWarning.tsx`.

- **`cpp_migration/src/commands/plugin/plugin_manage.cppm:193`** — Callers **translate the returned
  `exit_code`** to their environment — the module does not throw or exit.

- **`cpp_migration/src/commands/plugin/plugin_manage.cppm:272`** — A string is treated as a GitHub repo
  **only if it has exactly one slash and both sides are nonempty**.

- **`cpp_migration/src/commands/plugin_cmd.cppm:193-198`** — `/plugin manage` routes **directly** to the
  Installed (manage-plugins) tab with **NO textual list printed first**; it returns **only** the spawn
  metadata (`"UI:plugins:manage-plugins"`) and the dialog renders the plugins.

- **`cpp_migration/src/commands/add_dir.cppm:139-143`** — `--remember` is **effectively implicit**
  because `allowed_directories` persist via state persistence; the flag is **kept for CLI compatibility
  only**.

- **`cpp_migration/src/commands/terminal_setup.cppm:490-491`** — **TS `terminalSetup.tsx` never wraps
  executable instructions** — the `source <rc>` line must stay **plain** so copy-paste execution works.
  Only *paths* get OSC 8 hyperlinks.

- **`cpp_migration/src/commands/terminal_setup.cppm:194-200`** — The hyperlink gate lives **inside**
  `cc::utils::make_hyperlink()`, matching the TS early return. Divergence noted: unlike TS, this helper
  emits **ST as ESC-backslash** rather than BEL+ST — both are legal OSC 8 terminators.

- **`cpp_migration/src/commands/terminal_setup.cppm:433`** — Bail paths are **user-facing and linked**
  (the paths in the failure message are hyperlinked), so the error message is designed output, not
  debug text.

- **`cpp_migration/src/commands/insights.cppm:364-370`** — The HTML report is a **structurally faithful
  summary** of the TS HTML report (DOCTYPE, header, aggregate stat blocks, labelled bar-style lists per
  facet). The full interactive TS page (CSS theme, JS charts, timezone selector, copy buttons) is
  **intentionally NOT reproduced**. Do not treat the missing interactivity as a bug.

- **`cpp_migration/src/commands/insights.cppm:520-521`** — The facet-extraction prompt prefix is
  **verbatim from the TS source** and is **exported so callers (and tests) can assert the contract
  matches the TS feature**. Changing it breaks that assertion and the extracted facet schema.

- **`cpp_migration/src/commands/insights.cppm:555`** — Invalid schema mirrors the TS
  `extractFacetsFromAPI` **null-on-failure** behaviour rather than throwing.

- **`cpp_migration/src/commands/insights.cppm:586`** — The LLM extraction is invoked through an
  **injectable seam** so the command can be tested without a model.

- **`cpp_migration/src/commands/install.cppm:1-5`** — The native binary is its own **self-contained
  distribution**, so **(unlike the TS CLI) there is no npm installer to invoke** — `/install` reports
  build/install guidance instead of running an installer.

- **`cpp_migration/src/commands/review/ultrareview.cppm:17-19`** — The hosted **server-side
  billing/overage gate** (`checkOverageGate` + Extra Usage) was **removed along with the Anthropic cloud
  coupling**: a **local** multi-round review has **no remote quota to enforce**. Do not re-add a quota
  check.

- **`cpp_migration/src/commands/model.cppm:119`** — Command completion **cannot access AppState**
  because `complete()` is not given it — completions must be derived from command-local data only.

- **`cpp_migration/src/commands/model.cppm:325`** — `runtime_state` points to a `QueryEngine` but it
  **cannot safely** be used for that update path — the update goes through the AppState action system
  instead.

- **`cpp_migration/src/commands/tasks_cmd.cppm:336-340`** — `"complete"` is a **no-op** unless the task
  is in a state where manual completion is meaningful; there is no `manual_completion()` API, so the
  command emulates it by forcing the status.

- **`cpp_migration/src/commands/skills_cmd.cppm:356`** — `all_skills()` / autocomplete queries **rebuild
  from scratch** — there is no incremental invalidation.

- **`cpp_migration/src/commands/keybindings_cmd.cppm:344`** — Editor invocation uses **fork/exec via the
  bash module** — not a direct spawn.

- **`cpp_migration/src/commands/upgrade.cppm:23`**, **`desktop.cppm:20`**, **`mobile.cppm:21`**,
  **`chrome.cppm:20`**, **`heapdump.cppm:22`** — Module-internal helpers are **intentionally not
  exported** (module linkage). Importing them from outside is impossible by design, not an oversight.

- **`cpp_migration/src/keybindings/reserved_shortcuts.cppm:2-3`** — These shortcuts **cannot be
  overridden** by user bindings.


- **`cpp_migration/src/keybindings/template.cppm:44`** — Reserved shortcuts are **filtered out** of
  rebindable sets.

- **`cpp_migration/src/keybindings/match.cppm:123`** — `'meta'` is used as the field **proxy for super**
  in the `Modifiers` struct — there is no separate super field.

- **`cpp_migration/src/keybindings/match.cppm:147`** — The meta modifier is **ignored when matching**
  because of how escape sequences work — do not "fix" this to a strict comparison.


- **`cpp_migration/src/ui/app.cppm:1896-1901`** — Local slash commands in **simple UI mode do not need
  API access**, so missing credentials fall through to the simple UI rather than erroring.

- **`cpp_migration/src/ui/app.cppm:2013-2025`** — MCP tools are discovered **dynamically after server
  connection**: a provider callback is set so `build_request_body()` picks up newly-connected servers'
  tools **on every API call**. A second provider surfaces the **verbatim input schemas** so the request
  keeps nested shapes the simplified schema cannot represent.

- **`cpp_migration/src/bridge/core.cppm:681-684`** — `CcrClient` does not track inbound sequence
  numbers; the prior contract of **returning 0** is kept **until an inbound path is wired**. Callers
  must not treat 0 as a real sequence number.

- **`cpp_migration/src/bridge/core.cppm:239-251`** — A failed/empty token fetch **retries up to
  `kMaxRefreshFailures` times** at a fixed delay; the constant names the TS original.

- **`cpp_migration/src/bridge/core.cppm:898`** — The env-less bridge **does not buffer externally** —
  the no-op is the contract.

- **`cpp_migration/src/bridge/bridge_messaging.cppm:526`** — In daemon context (no callback registered)
  the failure is reported by return value, not thrown.

- **`cpp_migration/src/bridge/bridge_messaging.cppm:54`** — The dedup set is **bounded**: at capacity the
  **oldest entry is evicted** — so an ancient duplicate can be re-processed after enough traffic.

- **`cpp_migration/src/bridge/bridge_messaging.cppm:548`** — An unknown message subtype is answered with
  an **error** so the server does not wait forever.

- **`cpp_migration/src/bridge/session_api.cppm:300`** and **`:340`** — These are the documented protocol
  entry points for the session API.

- **`cpp_migration/src/cli/ccr_client.cppm:472`** — A dedicated guard exists for `token_` so
  `update_token()` **never contends** with a reader.

- **`cpp_migration/src/cli/sse_transport.cppm:80`** — A historical transport parsed `"https://"` but
  connected with **raw BSD sockets**, so the TLS assumption was false — this is why the current transport
  is separate.

- **`cpp_migration/src/cli/sse_transport.cppm:619`** — SSE event IDs **must not contain U+0000 NULL**.

- **`cpp_migration/src/cli/handlers/agents.cppm:118`** — A stale PID file is removed rather than treated
  as a live session.

---

## C. Cross-module / cross-file coupling — change one, silently break the other

### C.1 — Tag formats and string protocols shared across modules

- **`cpp_migration/src/ui/messages/collapse_background_bash.cppm:17-24`** — The tag-format contract: the
  **producers** are `local_agent_task.cppm:435`, `local_shell_task.cppm`, and `runtime_registry.cppm:766`
  (which write `<task_notification>`, `<status>`, `<summary>` — **underscored**), and the **consumer** is
  this collapse pass. Changing the emitters' spelling silently stops collapsing.

- **`cpp_migration/src/ui/messages/collapse_background_bash.cppm:44-48`** — The consumer-side constants
  that must match those producers.

- **`cpp_migration/src/tools/runtime_registry.cppm:450-453`** — `parse_lsp_action`'s action strings
  **mirror `lsp_action_name()` in `lsp_tool.cppm`**. Without them, `execute_lsp_tool` **silently falls
  through to `LspAction::Symbols`** for newer actions — a wrong answer, not an error.

- **`cpp_migration/src/services/lsp/diagnostic_registry.cppm:676-681`** — The JSON array shape here is
  **consumed by `lsp_tool.cppm`'s `parse_diagnostics`**, which requires: a JSON **array**, numeric
  severity **1-4**, `range.start`/`end` with `line`/`character`, `message`, optional `source`/`code`.

- **`cpp_migration/src/services/lsp/diagnostic_registry.cppm:700-702`** — The `severity` field being
  **always present and numeric** is the precondition for that consumer not falling back to its
  missing-severity default of **Info**.

- **`cpp_migration/src/services/lsp/client.cppm:117-121`** — Two modules export into the **same
  namespace** (`cc::services::lsp`); the local struct was renamed to `LspClientDiagnostic` to avoid an
  ODR collision with `diagnostic_registry`'s `Diagnostic`.

- **`cpp_migration/src/services/lsp/client.cppm:728-730`** — The registry injection is **additive** to
  the legacy callback; both consumers receive every notification.

- **`cpp_migration/src/utils/swarm_helpers.cppm:777-782`** — The envelope serializer is **replicated**
  from `team_helpers` because `detail::write_messages` is not exported. Two copies of the envelope
  format exist; they must agree.

- **`cpp_migration/src/utils/swarm_helpers.cppm:720-721`** — `json_quote` duplicates
  `team_helpers::detail::mailbox_json_escape` deliberately (to avoid coupling to that detail namespace).
  **The escaping switch must stay identical.**

- **`cpp_migration/src/utils/team_helpers.cppm:449-458`** — The authoritative RFC 8259 control-character
  escaping that the copy above must match.

- **`cpp_migration/src/utils/swarm_helpers.cppm:553-557`** — `permission_updates_json` is the exact
  wire field the leader fills and the worker consumes.

- **`cpp_migration/src/utils/swarm_helpers.cppm:941-943`** — The worker's `apply_updates` consumes that
  same array, persisting **verbatim**.

- **`cpp_migration/src/utils/swarm_helpers.cppm:566-577`** — The **frozen** request/response JSON text
  shapes shared between the leader TUI and the worker hook.

- **`cpp_migration/src/ui/app_team_projection.cpp:464-467`** — The discriminator substrings
  (`"type":"permission_request"`) come from the **frozen stage-A protocol**; the generic
  `"loom:permission"` tag grep is **intentionally NOT used** so only real envelopes parse. Changing the
  producer's field name breaks the leader's inbox projection silently.

- **`cpp_migration/src/ui/messages/user_text_message.cppm:429-433`** — The B5 `UserCommandMessage`
  background rule, contrasted explicitly with `UserPromptMessage` (which **does** swap to
  `messageActionsBackground` on selection). The two message types intentionally differ.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:78-85`** — `VirtualList`'s own
  `VisibleRow` struct vs `messages_list`'s; the **caller** owns the conversion function. The two types
  must stay structurally compatible at that boundary.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:540-546`** — The nearest-match computation
  depends on `jh.find_visual_top_for_row(row_idx)` — the row-height model in this module and the row
  indices produced by `messages_list` must agree.

- **`cpp_migration/src/ui/messages/messages_list.cppm:1842-1844`** — `build_visible_rows` filters; the
  virtual-list search engine navigates **within that filtered set**. If the filter changes to also
  reorder, the navigation indices go stale.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2351-2358`** — The faithful renderers named here
  (`RenderUserPromptMessage`, `RenderAssistantTextMessageFaithful`, `RenderThinkingMessageFaithful`,
  `RenderSystemTextMessageFaithful`, `RenderFaithfulToolUseMessage`) live in **five other modules**, and
  the dispatcher's shape strings must match the shapes the projection in `app.cppm` emits.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2523-2527`** — The **bridge** between
  `ToolResultOptions` (`ui/messages/…`) and `ToolResultFaithfulData`
  (`message_tool_result.cppm`) — two models, one conversion, and changing either struct's fields
  requires editing this bridge.

- **`cpp_migration/src/ui/messages/messages_list.cppm:107-108`** — The FTXUI header-availability
  constraint that any new include in this module must respect.

- **`cpp_migration/src/ui/app.cppm:453-465`** — The `MessageDisplayEntry` fields set here are read by
  `repl_screen`'s dispatcher, which **checks `is_tool_use` FIRST**. `app.cppm` and `repl_screen.cppm`
  must agree on that sentinel or committed tool results are routed to the wrong renderer.

- **`cpp_migration/src/ui/messages/tool_use_message.cppm:168-171`** — The status vocabulary
  (`"pending" | "running" | "success" | "error"`, `"cancelled"` tolerated) is the **shared projection
  contract**; `app.cppm` (G2) populates it and this helper (G1) reads it.

- **`cpp_migration/src/ui/messages/user_text_message.cppm:295`** — The pointer glyph is **not** defined
  here — it lives in `cc.ui.design.figures::kPointer`. A literal here reintroduces the
  "prefix glyph three fights" bug.

- **`cpp_migration/src/ui/messages/system_text_message.cppm:244-249`** — The glyph names were
  **historically** local (`kReferenceMark`, `kTeardropAsterisk`, `kSystemBlackCircle`) and are now
  mapped to the shared `figures::` symbols; the mapping is documented so old names are still
  recognisable.

- **`cpp_migration/src/ui/design_system/figures.cppm:5-9`** — The single-source-of-truth rule for glyphs
  across 10+ `.cppm` render sites.

- **`cpp_migration/src/ui/design_system/figures.cppm:187-193`** — The legacy-symbol retention
  (`kBridgeReadyIndicatorLegacy`) is an explicit one-release compatibility window — **it must be
  removed after that release**.

- **`cpp_migration/src/ui/design_system/theme_provider.cppm:344-346`** — The color-name lookup accepts
  **both camelCase and snake_case** spellings of every TS theme field, so both TS-derived and
  C++-idiomatic call sites resolve. A new field must be added in both spellings.

- **`cpp_migration/src/ui/mcp/mcp_server_details.cppm:139-141`** — `status_badge_decor` is a deliberate
  **duplicate** of `status_badge` to avoid cross-module inline-ordering issues — **the two must be kept
  in sync by hand**.

- **`cpp_migration/src/ui/components/partial_completions.cppm:448-450`** — The highlight color choice is
  explicitly **aligned with `code_highlight.cppm`'s `type_name = magenta/cyan` convention**.

- **`cpp_migration/src/ui/components/file_edit_tool_diff.cppm:184-195`** — The conversion
  `utils::file_edit::PatchHunk` → `structured_diff::StructuredPatchHunk` is the coupling point between
  two modules; the `header` field is left empty because `RenderHunkHeader` builds it from the
  start/line counts.

- **`cpp_migration/src/ui/components/structured_diff.cppm:306-315`** — The **reason** the char-level LCS
  is duplicated here rather than reused: `utils/file_edit`'s Myers is **line-level and not exported at
  character granularity**. `Deferred(#ui7-word-diff)` tracks consolidating it.

- **`cpp_migration/src/ui/prompt/fuzzy_rank_nucleo.cppm:31-34`** — The scoring constants are
  **mirrored from `cc.utils.file_index`** so the two scorers agree **without a hard import**. Editing one
  side alone makes the two disagree.

- **`cpp_migration/src/ui/prompt/fuzzy_rank_nucleo.cppm:5-15`** — The `{0..3}` bucket contract is shared
  with **`app.cppm`'s tier-offset model** (alias +1, skill +4, plugin +6). Widening the range here
  silently reorders autocomplete categories.

- **`cpp_migration/src/ui/prompt/fuzzy_rank_nucleo.cppm:131-139`** — The verbatim contract (empty query
  → 1000; exact → 0; range `{0,1,2,3}`) and the **-1000** hidden-command hatch set at the call site.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:1145`** and **`messages_list.cppm:542`** — The 24-char
  uuid prefix anchor is a shared convention between the unseen-divider computation and the divider
  index lookup.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2153-2163`** — The prefix-glyph precedence is
  documented identically in `mode_indicator.cppm` and here; both implement the same TS ordering
  (`viewingAgentName` before `bash`).

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2570`** — The suppression rules are a **mirror of TS
  `REPL.tsx`**; the band numbers here must match `dialog_system.cppm`'s `priority_for`.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:3061-3068`** — The `InputMode` → footer
  `PromptInputMode` assignment is a direct cast only because the two are now the **same unified type**
  (`cc::ui::common::PromptInputMode`). Splitting the type again requires restoring a translation table.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:4097-4104`** — The dialog-slot predicates are checked
  **inside `DispatchDialogQueueEvents()`**, and the per-slot helpers are **not re-exposed** here **to
  avoid a duplicate definition with `dialog_queue_render`**.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:3880-3882`** — The one-shot guard is required because
  the bash/edit/write panels (in yet other modules) **both** invoke `on_abort` and `on_decide(Abort)` on
  one Esc.

- **`cpp_migration/src/ui/screens/log_selector.cppm:366-372`** — The session-card style is
  **hand-synced** with `resume_screen.cppm`'s `RenderSessionCard`; the comment exists specifically so
  the two stay aligned.

- **`cpp_migration/src/ui/screens/log_selector.cppm:68`** — The designated import site for shared
  tokens / UI8 trust primitives.

- **`cpp_migration/src/ui/screens/doctor_screen.cppm:165`** — The version-lock info shape comes from the
  TS `Doctor.tsx` `VersionLockInfo` structure.

- **`cpp_migration/src/ui/dialogs/all_renderers.cppm:1-11`** — **TEST CONTRACT:** unit tests alias
  `namespace dr = cc::ui::dialogs::all_renderers;` and call `dr::RenderXxx(...)` / `dr::HandleXxxEvent(...)`.
  This module re-exports **every** such function via explicit `using` declarations so lookup resolves
  **without ambiguous namespace-qualification errors**. A new renderer added to a sub-module **must also
  be re-exported here** or the tests stop finding it.

- **`cpp_migration/src/ui/dialogs/dialog_default_renderers.cppm:635-646`** — The unregistered dialog
  types list is the counterpart to the registry; adding a trigger path without registering a renderer
  produces a fallback placeholder.

- **`cpp_migration/src/ui/dialogs/dialog_system.cppm:304-311`** — The typing-suppression predicate is
  derived from `priority_for(type)` — the band table and the suppression rule are one contract.

- **`cpp_migration/src/ui/dialogs/triggers.cppm:485-496`** — The `CommandMetadata` strings
  (`"UI:model-picker"`, `"UI:plugins:*"`, `"MANAGED_SETTINGS_SECURITY"`) originate in the **command
  layer** (e.g. `command_registry.cppm`'s `"UI:help"`, `plugin_cmd.cppm`'s
  `"UI:plugins:manage-plugins"`). This dispatcher is the single consumer; a command emitting a new tag
  with no case here silently does nothing.

- **`cpp_migration/src/commands/command_registry.cppm:134-141`** — The producer of the `"UI:help"`
  tag the dispatcher handles.

- **`cpp_migration/src/commands/plugin_cmd.cppm:184-201`** — The producers of
  `"UI:plugins:discover-plugins"` / `"UI:plugins:manage-plugins"`.

- **`cpp_migration/src/ui/dialogs/dialog_system.cppm:823`** — `DialogType` is derived from the payload
  variant via `type_of()`, which feeds `slot_of()` and then `priority_for()` — a four-step contract
  (`DialogPayloadVariant` → `type_of()` → `slot_of()` → band).

- **`cpp_migration/src/ui/dialogs/triggers.cppm:5-8`** — The same four-step membership chain is named
  explicitly as the contract these helpers must satisfy.

- **`cpp_migration/src/ui/layout/fullscreen_layout.cppm:86`** — The component-lifetime guard is
  **copied verbatim from `permission_rule_list.cppm`** — a shared pattern, duplicated.

- **`cpp_migration/src/ui/layout/fullscreen_layout.cppm:358-367`** — The notice ordering matches TS
  exactly; see A.9.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:1160`** — The voice-indicator priority is
  mirrored from `Notifications.tsx`; "idle renders null and must not add a row."

- **`cpp_migration/src/ui/prompt/placeholder_cascade.cppm:143-175`** — The cascade layers are numbered
  L1-L4 with TS line references; inserting a layer renumbers the rest.

- **`cpp_migration/src/ui/prompt/at_attachments.cppm:119-128`** — The `@`-mention resolution order is a
  contract: agent mention (AT-10) → MCP resource `@server:uri` (AT-11) → file. **A token with a colon
  never falls through to file** — reordering these changes what `@foo:bar` means.

- **`cpp_migration/src/ui/autocomplete_sources.cppm:41`** — DM/teammate and named-agent source
  construction.

- **`cpp_migration/src/ui/autocomplete_sources_impl.cpp:263`** — Agent suggestions match on
  `agentType` **or** `displayText`.

- **`cpp_migration/src/ui/agents/agent_wizard.cppm:67-73`** — The adapter types bridging
  `wizard_dialog`'s callbacks to this wizard's props+component pattern.

- **`cpp_migration/src/ui/teams/team_details_dialog.cppm:42-46`** — The usage-stats `Element` is
  **pre-rendered by the caller** and passed in via `usage_stats_element`; importing
  `cc.ui.dialogs.usage_dialog` was avoided to keep the file standalone. Callers that forget to pass it
  get no real numbers.

- **`cpp_migration/src/ui/teams/teams_overview.cppm:57`** — The `Member` / `Activity` / avatar helpers
  are **functionally equivalent to** the UI13 versions but **independently consumed standalone**.

- **`cpp_migration/src/ui/tasks/task_list_view.cppm:44`** — A local POD is deliberately kept for display
  so the view is **decoupled** from `cc.tasks.*`.

- **`cpp_migration/src/ui/tasks/task_details_dialog.cppm:181`** — Temporary input buffers are **kept in
  sync with the data** for rendering — two sources of truth in the interim.

- **`cpp_migration/src/tools/agent_tool.cppm:220-221`** — The `using`-imports at the module root are
  the seam between `agent_tool` and its submodules; `TODO(agent-split)` tracks the ambiguous placement.

- **`cpp_migration/src/state/teammate_view_helpers.cppm:3`** — **Duplication note:** this logic also
  exists in `hooks/swarm_hooks.cppm` and `hooks/teammate_view_auto_exit.cppm` — three copies to keep in
  sync.

- **`cpp_migration/src/skills/bundled/skill_keybindings.cppm:26`** and **`:227`** — Static data and
  prompt sections **kept in sync with TS** `keybindings/schema.ts` and the `SECTION_*` constants.

- **`cpp_migration/src/skills/bundled/loom_in_chrome.cppm:41`** — The base Chrome system prompt is
  **kept in sync with the TS source file** by hand.

- **`cpp_migration/src/skills/load_skills_dir.cppm:2288-2298`** — The file-access hook is the seam that
  lets `cc_tools` trigger skill discovery **without depending on `cc_skills`**; `cc_tools` calls the
  hook, `cc_skills` registers it.

- **`cpp_migration/src/skills/loom_api_content.cppm:32-35`** — The model-var map must match the TS
  `SKILL_MODEL_VARS`; the `{{OPUS_ID}}`-style placeholders in the prompt fragments are resolved from it.

- **`cpp_migration/src/ui/screens/log_selector.cppm:593-595`** — Out-of-line declarations kept for
  readability in a single-module world.

### C.2 — Breadcrumbs that are unresolvable without a sibling comment

These `TS REF:` pointers give only a line number or a bare filename fragment. They are harmless while
`src/` exists; after deletion they read as dangling references. Recorded here so the intended target is
preserved in prose — and so a future reader knows **which** TS file the number belonged to.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:540`** — `// TS REF: L737-758 nearest-match
  by abs(origin + offsets[matches[k]] - curTop)`. **The filename is absent from this comment** and is
  recoverable only from the module-level block at **`virtual_message_list.cppm:421`**, which names
  `src/components/VirtualMessageList.tsx`. The referenced logic is: *find the nearest search match to
  the current scroll position by minimizing `abs(origin + offsets[matches[k]] - curTop)`* — i.e. the
  match whose **visual top** is closest to the current scroll top. The C++ implementation at
  `virtual_message_list.cppm:539-546` does the same via
  `jh.find_visual_top_for_row(row_idx)` and `std::abs(row_top - origin)`.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:419-426`** — The sibling block that makes
  the above resolvable: it names `src/components/VirtualMessageList.tsx` and lists
  `L702-780 setSearchQuery`, `L650-694 step(delta)`, `L797-816 warmSearchIndex`. **This is the only
  place in the file that supplies the filename** — deleting it would orphan every bare `L###` reference
  below.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:604-605`** — `// TS: L692-693 placeholder =
  delta < 0 ? prefixSum[ptr+1] : prefixSum[ptr]+1` — bare line numbers, resolvable only via the block
  above. Recorded divergence: the C++ uses `prefixSum[ptr] + 1` (first occurrence in this message)
  **unconditionally**, so the "current occurrence" badge can differ from TS when navigating backwards.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:585`**, **`:590`**, **`:605`**,
  **`:645-646`**, **`:929`**, **`:945`**, **`:978`** — Further bare `L###` references whose filename
  comes only from the `:421` block (`VirtualMessageList.tsx`) or the `:645`/`:978` comments which name
  `REPL.tsx` inline.

- **`cpp_migration/src/ui/messages/api_error_message.cppm:207-225`** — `// TS REF: L103
  retryInSecondsLive === 1 ? "second" : "seconds"` — a bare line number with **no filename in this
  comment**. The filename is supplied by sibling comments in the same file
  (`api_error_message.cppm:55`, `:81`, `:261`, `:293`), which name **`SystemAPIErrorMessage.tsx`**. The
  referenced logic is the singular/plural choice for the retry countdown: when the live retry countdown
  equals 1, the text says `"second"`, otherwise `"seconds"`.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:477`** — `// TS REF: Messages.tsx expandedKeys (L563)`
  — here the filename *is* present but the bare `L563` is only meaningful with it; the referenced
  concept is the `Set` of expand keys the user has opened, which drives whether a row renders expanded.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:1520`** — References `useUnseenDivider onScrollAway`
  with no file path; the divider semantics are otherwise documented only in TS.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:1131-1134`** — Explains that the `leading_element`
  must render inside the `yframe` so it scrolls; the TS side of this is a `Messages.tsx` layout detail
  recorded only in that file.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2000`**, **`:2045`** — Behaviour of the `'!'`
  mode trigger, referenced without a TS path.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2158`** — `PromptInputModeIndicator.tsx line 82` — a
  filename with a bare line; the precedence rule it encodes is restated in prose in
  `mode_indicator.cppm:13-16`.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:4232-4234`** — The `Ctrl+L` redraw shape, referenced
  by TS path plus line only.

- **`cpp_migration/src/ui/dialogs/sandbox_settings.cppm:603`**, **`:823`**, **`:528`**, **`:381`**,
  **`:309`** — Several `TS REF: Sandbox*Tab.tsx:<range>` pointers **with no prose**. The only content
  recoverable is that the dependency-check list is Linux-focused (`bwrap`, `socat`, `seccomp`) and that
  the dialog **starts on the Dependencies tab when there are errors**, otherwise on the Mode tab.
  Everything else about those tabs lives only in the TS files.

- **`cpp_migration/src/ui/tools/tool_ui_longtail.cppm:126`**, **`:157`**, **`:187`**, **`:211`** —
  `TS REF:` pointers to Simple tools / ComputerUse / WebBrowser / AskUserQuestion with the note that
  their **output IS visible** in the transcript. The visible-vs-hidden classification is the only
  recoverable fact and it **is** stated; the rest is not.

- **`cpp_migration/src/ui/design_system/figures.cppm:313`**, **`:343`**, **`:385`**, **`:432`**,
  **`:469`**, **`:508`**, **`:519`**, **`:540`** — Bare `L###` references into
  `node_modules/figures/index.js` and `figures/index.js`. The **npm package is not part of the deleted
  tree**, but these line numbers are meaningless without it. The recoverable facts are stated in prose:
  `mainSymbols = {...common, ...specialMainSymbols}`, `fallbackSymbols = {...common,
  ...specialFallbackSymbols}`, the default export shape, and
  `replaceSymbols(string, {useFallback = !shouldUseMain})`.

- **`cpp_migration/src/utils/parse_references.cppm:81`** — `dropped (TS filter semantic at L74)` — a
  bare line number; the sibling comment at **`:77`** supplies the filename (`src/history.ts`) and the
  full regex. The rule itself is fully restated in C++ at `:110` (`id <= 0` is dropped), so no
  information is lost.

- **`cpp_migration/src/ui/app_autocomplete.cpp:1242`** — References `text_input.cppm L586` — an
  **intra-C++** line reference, not a TS one. Intra-file intra-repo line references like this one are
  fragile for the same reason: they were not covered by the `TS REF` convention and will drift
  independently.

- **`cpp_migration/src/ui/messages/message_image.cppm:223`** — References `repl_screen.cppm L825-831`
  (a C++ file, not TS) — the same fragility.

- **`cpp_migration/src/ui/dialogs/cost_threshold_dialog.cppm:24`** — References an inline lambda in
  `repl_screen.cppm`; the contract is duplicated across three sites (this file, `dialog_system.cppm`,
  and `repl_screen.cppm`).

- **`cpp_migration/src/tools/runtime_shared_utils.cppm:13-20`** — A list of **intra-C++** `L###`
  references recording where each helper used to live (`runtime_shell_quote L387–395`,
  `escape_xml L557–571`, etc.). These are historical breadcrumbs into a file that has since been split;
  they are not TS references at all.

### C.3 — Ordering / enumeration contracts

- **`cpp_migration/src/hooks/permission_resolver.cppm:34-35`** — `Decision` ordinals are persisted; see
  B.1.

- **`cpp_migration/src/commands/compact.cppm:29-36`**, **`brief.cppm:24-26`**, **`clear.cppm:29-32`**,
  **`cost.cppm:29`**, **`plan.cppm:29`** — Action-type ordinals must track `store.cppm`'s
  `cc::state::ActionType` ordering.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:244-266`** — The rainbow token set is
  **7 base + 7 shimmer = 14 individual fields**, constructed individually so each theme variant can
  override independently. Collapsing them into an array breaks per-variant overrides.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:283`** — Concrete palettes are **copied
  verbatim from the TS `rgb()` values** — not derived.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:791`**, **`:907`** — The light and dark ANSI
  palettes are indexed **0..15**; the `ansi:N` theme spelling indexes into this palette, so the order is
  load-bearing.

- **`cpp_migration/src/ui/design_system/theme_provider.cppm:213`** — `ansi:N` means **palette16 index
  (0..15)**.

- **`cpp_migration/src/ui/layout/logo_v2.cppm:358-361`** — The notice render order is fixed by TS.

- **`cpp_migration/src/utils/swarm_helpers.cppm:932-939`** — Only **whole-tool** grants (`rule_content`
  empty) match in `allows()`; content-scoped rules are intentionally excluded — see B.5.

- **`cpp_migration/src/services/mcp/channel_permissions.cppm:296-302`** — The three relay conditions and
  the requirement that **both** capabilities be declared.

- **`cpp_migration/src/tools/mcp_classify.cppm:759-771`** — The classification precedence, with errors
  overriding everything.

- **`cpp_migration/src/tools/readonly_validation.cppm:308-314`** — Multi-word command lookup is tried
  **before** single-word (`git diff`, `git stash list`, …). Reversing the order mis-classifies compound
  git commands.

- **`cpp_migration/src/tools/destructive_command_warning.cppm:50-51`** — Patterns are ordered **by
  sensitivity**; the first match wins, so reordering changes which warning is reported.

- **`cpp_migration/src/ui/prompt/at_attachments.cppm:112-128`** — The resolution order documented above.

- **`cpp_migration/src/ui/app.cppm:542-553`** — Block projection order: consecutive text blocks merge,
  any non-text block flushes; see B.8.

- **`cpp_migration/src/ui/dialogs/cost_threshold_dialog.cppm:78-82`** — Arrow keys are no-ops because
  the index is pinned at 0.

- **`cpp_migration/src/ui/messages/scroll_keybindings.cppm:369`** — Wheel events are routed via
  `computeWheelStep`; the FSM here handles the keyboard side, and the two must agree on step size.

- **`cpp_migration/src/services/auth/provider_selector.cppm:5-9`** — The provider priority order,
  documented above in A.5.

- **`cpp_migration/src/services/lsp/manager.cppm:313-319`** — Plugin-cache clearing order relative to
  capability refresh.

- **`cpp_migration/src/migrations/schema_versions.cppm:132-143`** — The write ordering
  (temp-on-disk **before** rotating the primary into `.bak`); see B.6.

- **`cpp_migration/src/migrations/config_orchestrator.cppm:556-563`** — The files-then-version commit
  order; see B.6.

- **`cpp_migration/src/query/wire_openai.cppm:363-369`** — Tool messages before the user message; see
  A.4.

- **`cpp_migration/src/services/auth/sigv4.cppm:400-402`** — `host`/`date` must be in
  `canonical_headers` before `build_signed_headers_list`; see A.5.

- **`cpp_migration/src/utils/hyperlink.cppm:165-174`** — The percent-encoding safe set; see B.8.

- **`cpp_migration/src/ui/screens/doctor_screen.cppm:691-699`** — Rule-order sensitivity: a `deny` after
  a `"**"` allow can never fire.

- **`cpp_migration/src/ui/prompt/mode_indicator.cppm:13-16`** — The prefix-glyph precedence; see A.10.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2863-2872`** — The margin exceptions (tool
  results, user continuations) relative to the default `addMargin=true`.

- **`cpp_migration/src/ui/messages/messages_list.cppm:3464`** and **`:1819`** — The `N new messages`
  divider is inserted **BEFORE** the row, not after.

- **`cpp_migration/src/ui/messages/messages_list.cppm:3592-3601`** — The unseen-divider stability guard
  hashes both a "present?" bit and the count so either change triggers a rebuild; `streaming_tail_row`
  is deliberately excluded.

---

## D. Explicitly-marked divergence blocks

These comments carry an explicit divergence marker ("TS vs CPP divergence", "intentional",
"deliberately", "divergent", "NOTE:", "IMPORTANT:", "DIFFERENCE:") as a *block*. Most of the
substantive ones are transcribed in full in section A; the value of this section is the **inventory**:
it tells you every place the C++ authors stopped to say "we know this differs." Where a block's content
is already stated above, the entry is a one-line pointer so the inventory stays complete without
duplicating prose. Entries marked **(new)** are decisions recorded *only* here.

### D.1 — Named divergence blocks (the loudest markers)

- **`cpp_migration/src/ui/messages/collapse_background_bash.cppm:15-24`** — **(new)** The only block in
  the tree that uses the exact phrase **`TAG-FORMAT NOTE (TS vs CPP divergence — intentional)`**. It is
  the clearest worked example of the pattern: name the TS behaviour, name the CPP behaviour, name *who
  produces the CPP format*, then declare the constants below the single source of truth for the
  decision. Worth preserving as the template.

- **`cpp_migration/src/state/store.cppm:944-956`** — `NOTE: this is an intentional no-op.` See B.7.

- **`cpp_migration/src/state/store.cppm:909-911`** — Reducer no-ops pending explicit payload semantics.
  See B.7.

- **`cpp_migration/src/hooks/notifs/remaining_notifs.cppm`** — **(new as an inventory)** The densest
  cluster of divergence markers in the codebase: **six `SLOT FALLBACK (intentional)` blocks** at lines
  **188, 237, 302, 349, 527, 668**, plus a summary at line **23**
  (`ModelMigration → SLOT fallback (intentional)`). Each names the specific TS runtime signal that has
  no C++ accessor (`getCurrentInstallationType() === 'development'` + `isInBundledMode()`; a
  recent-`<3s` migration read; a subscribe/emit pattern; a settings-with-all-errors query;
  `isLoomAISubscriber()`) and declares the slot the honest injection point until that producer exists.
  **This is the reference for how the port records a missing capability**: not "TODO", but "here is
  the signal TS read, here is why we cannot read it, here is where the real implementation plugs in."

- **`cpp_migration/src/query/wire_openai.cppm:668-698`** — `STREAMING CONTRACT` + `KNOWN LIMITATION`.
  See A.4.

- **`cpp_migration/src/query/wire_openai.cppm:40-42`**, **`:89-91`**, **`:468`**, **`:496-501`**,
  **`:530-534`** — The `LOSSY:` markers (stateless backend; base64 not inlined; `top_k` dropped;
  computer-tool shape ignored; thinking ignored). See A.3.

- **`cpp_migration/src/query/wire_anthropic.cppm:13`** — **(new)** `/// What the backend intentionally
  does NOT own (the engine still does):` — this is the *seam declaration* for the wire-backend
  abstraction. It is the authority on which responsibilities may not migrate into a backend.

- **`cpp_migration/src/query/wire_anthropic.cppm:293`** — `Mapping notes (all deliberate, to keep the
  engine's behaviour)`. See A.3.

- **`cpp_migration/src/query/wire_protocol.cppm:19-21`** — `Design note: the interface is intentionally
  narrow and value-oriented`. See A.4.

- **`cpp_migration/src/services/analytics.cppm:4-20`** — `## Why append-only and why resilient`, plus
  `This is the local telemetry the decoupling plan chose to KEEP`. See A.6.

- **`cpp_migration/src/ui/layout/yoga.cppm:20-36`** — `intentionally a SMALL single-pass flexbox` +
  `Deliberately NOT ported (none are reachable from the C++ UI as of this audit)`. See A.9.

- **`cpp_migration/src/ui/common/ui_types.cppm:81-93`** and **`:222-237`** — `UNIFIED CANONICAL ENUM —
  replaces incompatible definitions scattered across the codebase`. See A.1/A.2.

- **`cpp_migration/src/vim/vim_types.cppm:8-18`** — `Replaces 5 incompatible local VimMode definitions`
  with a per-file list. See A.1.

- **`cpp_migration/src/services/prompt_suggestion/prompt_suggestion.cppm:394-401`**, **`:444-452`**,
  **`:656-657`**, **`:679-683`** — `intentionally NOT produced … This is an explicit, documented gap`;
  `CONSERVATIVE PLACEHOLDER … does not claim parity`; `intentionally conservative (false-stops are
  safe)`. See B.9.

- **`cpp_migration/src/services/lsp/LSPServerInstance.cppm:307-313`** — `failing to compile is
  preferable to silently returning T{}`. See B.4.

- **`cpp_migration/src/utils/tool_deny_rules.cppm:30-39`** — **(new)** The deleted hosted-connector
  prefix branch, with the sharpest rationale in the tree: *"A prefix test that can never be true is not
  a safety net, it is a place for a future reader to believe something is handled that is not."* Worth
  quoting whenever a "defensive" branch is proposed for code that cannot reach it.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:314-317`** — `KNOWN non-rendered TS artifact
  … faithful-in-spirit, avoids a destructive visual change`. See A.9.

- **`cpp_migration/src/ui/design_system/design_tokens.cppm:344-345`**, **`:306-307`**, **`:563`** —
  `corrected from …`, `corrected sky-blue → TS lavender`, `ported verbatim … NOT a matrix
  approximation`. See A.9.

- **`cpp_migration/src/ui/design_system/figures.cppm:189-191`** — `previous CPP value was wrong`.
  See A.9.

- **`cpp_migration/src/ui/design_system/figures.cppm:59-69`** — `The CPP InputMode enum used to have
  extra values … From this commit on, the prompt glyph only has TWO rendered variants`. See A.2.

- **`cpp_migration/src/ui/prompt_input.cppm:443-447`** — `NOTE: VimHandler removed`. See A.1.

- **`cpp_migration/src/ui/components/text_input.cppm:2354-2361`** — `IMPORTANT: We do NOT strip the
  leading '!' from text_ here.` See A.2.

- **`cpp_migration/src/ui/design_system/figures.cppm:271-275`** — `IMPORTANT: The '!' is a transient
  mode trigger.` See A.2.

- **`cpp_migration/src/ui/prompt/prompt_input_footer.cppm:520`** and **`:1111`** — `There is no TS
  equivalent — this is a CPP-only enhancement`. See A.10.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:3216-3219`** — `NOTE: TS upstream does NOT render a
  brand pill in the footer`. See A.8.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:2249-2256`** — `We intentionally do NOT declare the
  prompt caret position here`. See A.8.

- **`cpp_migration/src/ui/screens/repl_screen.cppm:1226-1229`** and
  **`ui/messages/message_image.cppm:222-226`** — `deliberately do NOT stuff ib.data into alt_text` /
  `deliberately NO "Alt:" line … Removed 2026-07-04 per spacing bug report`. See A.8.

- **`cpp_migration/src/ui/messages/messages_list.cppm:2351-2367`**, **`:2690`**, **`:2523-2524`**,
  **`:2869-2872`**, **`:1270-1271`**, **`:2979-2986`**, **`:3602-3604`** — The `divergent` /
  `intentionally NOT` cluster in the message renderer. See A.8.

- **`cpp_migration/src/ui/messages/tool_use_message.cppm:636-639`**,
  **`assistant_text_message.cppm:478-479`**, **`thinking_message.cppm:567-569`** — `the divergent FTXUI
  reimplementation … all invented chrome`. See A.8.

- **`cpp_migration/src/ui/messages/user_message.cppm:12`** and **`:87`** — **(new)** The 10K paste cap
  must be applied in **both** the faithful path and the `UserMessageData` **divergent-envelope** path;
  the comment exists solely to keep the two in agreement.

- **`cpp_migration/src/ui/dialogs/cost_threshold_dialog.cppm:67`** and
  **`dialog_system.cppm:397`** — `P0 CONTRACT — DO NOT add fabricated actions
  (Continue/Reset/Quit)`. See A.10.

- **`cpp_migration/src/ui/dialogs/dialog_default_renderers.cppm:263-278`** — **(new)** `P0x3 contract.
  This renderer previously contained fabricated [chrome]` — a divergence from an earlier *C++* mistake,
  not from TS, with the contract restated so it cannot recur.

- **`cpp_migration/src/ui/dialogs/dialog_default_renderers.cppm:641-646`** — `intentionally NOT
  registered … registering a stub renderer would be dead code / a misleading placeholder`. See A.10.

- **`cpp_migration/src/ui/dialogs/dialog_launchers.cppm:265-270`** — `Deliberately small — setup dialogs
  MUST NOT access the full AppState`. See A.10.

- **`cpp_migration/src/ui/dialogs/plugin_dialog.cppm:770-771`** and **`triggers.cppm:501`** — `TS
  reference has no 5-card dashboard` / `no card dashboard`. See A.10.

- **`cpp_migration/src/ui/dialogs/mcp_dialogs.cppm:1249-1251`** — `Enter no longer toggles booleans —
  that was a UX bug`. See A.10.

- **`cpp_migration/src/ui/tools/tool_ui_registry.cppm:6`** — **(new)** `The TS reference puts these as
  methods on each tool class` — the C++ uses free functions instead; this is the only record that the
  dispatch shape differs from TS by design.

- **`cpp_migration/src/ui/prompt/prompt_paste_handler.cppm:121`** — **(new)** The paste-preview styling
  diverges from what `chalk.inverse` produces in the TS reference: **no bold** — TS doesn't [use it].

- **`cpp_migration/src/ui/agents/agent_details_dialog.cppm:148`**, **`:735-737`**, **`:853`** — `NOTE:
  caller must wrap this in a TrustDialog critical-confirm`. See B.9.

- **`cpp_migration/src/ui/messages/messages_list.cppm:26-31`** — **(new)** The stated **migration
  procedure**: UI20 `design.tokens` / `design.primitives` are not yet materialised; `themed_text` +
  `themed_box` are working stand-ins; *"every palette lookup goes through the small inline helpers
  `palette::*()` so that swapping the real modules is a one-line grep."*

- **`cpp_migration/src/ui/messages/messages_interactions.cppm:55-58`** — `UI21 messages_list should later
  expose a MessageRowPayload that *contains* a MessageMetadata`. See B.8.

- **`cpp_migration/src/ui/dialogs/all_renderers.cppm:1-11`** — `TEST CONTRACT`. See C.1.

- **`cpp_migration/src/ui/mcp/mcp_server_details.cppm:140`** — `Standalone re-declaration of
  status_badge to avoid cross-module inline ordering issues`. See C.1.

- **`cpp_migration/src/ui/components/text_input_widget.cppm:480`**, **`:855-865`** — `widget
  historically uses Green + bold for prefix` and the widget-local vim no-ops (`p`/`P` paste, `Ctrl+R`
  redo). See A.1.

- **`cpp_migration/src/ui/components/fast_icon.cppm:37`** and **`pr_badge.cppm:67`** — Documented
  non-implementations (theme system not reached; FTXUI has no `Link` component).

- **`cpp_migration/src/ui/layout/fullscreen_layout.cppm:208-214`**, **`:196-206`**, **`:281-285`** —
  `intentionally decouple "state change" from "scroll mutation"`; the sticky-prompt 3-state model and
  the `'clicked'` sentinel; `This FTXUI build does not expose a truncation() decorator`. See A.10.

- **`cpp_migration/src/ui/teams/swarm_collaboration_view.cppm:24`** — **(new)** A deliberately humorous
  comment whose actual content is the import note *below* it; the marker exists so the note is not
  skimmed past. Also **`:533`**: `opts.scroll_transcript` / `opts.auto_scroll` are declared but not
  acted on.

- **`cpp_migration/src/bridge/config.cppm:243-245`** and **`constants/product.cppm:15-18`** /
  **`ui/dialogs/about_dialog.cppm:77-79`** — `the upstream vendor's host must not be renamed into one
  that does not resolve`. See A.10.

- **`cpp_migration/src/hooks/notifs/auto_mode_unavailable.cppm:38-39`** — `No notification surface is
  wired in the C++ migration; the body is intentionally empty (parity with the flag-off TS branch).`
  See D.1 above for the family.

- **`cpp_migration/src/hooks/permission_resolver.cppm:4`** and **`:332-335`** — `intentionally
  lightweight and UI-agnostic`; `We intentionally avoid pulling in the full JSON module so this resolver
  can be used by the earliest startup paths`. See B.1.

- **`cpp_migration/src/hooks/shell_hooks.cppm:547-550`** — The hooks READ-cascade rationale. See B.6.

- **`cpp_migration/src/migrations/migration_registry.cppm:46`** — `schema v1 is deliberately minimal`.
  See B.6.

- **`cpp_migration/src/state/persistence.cppm:104-105`**, **`memdir/paths.cppm:161-165`**,
  **`constants/paths.cppm:106-112`**, **`config/settings.cppm:203-204`** — `deliberately not persisted`;
  `Uses the WRITE resolution on purpose`; `Deliberately narrower than the read cascade`. See B.6.

- **`cpp_migration/src/query/config.cppm:38-41`** — `Removed 2026-06-15. … do not revive the unused
  prototype.` See B.7.

- **`cpp_migration/src/query/query_engine.cppm:3146-3150`**, **`tools/agent_runtime.cppm:3745`**,
  **`tools/agent_runtime.cppm:3647`**, **`tools/agent_memory.cppm:5-10`** — `NOTE:` markers recording
  what is *not* here and where it lives. See A.11.

- **`cpp_migration/src/tools/migrated-edge-case cluster`** — `tools/agent_runtime.cppm` lines
  **1635, 1639, 1647, 1670, 3661, 3701, 3736, 3892, 3907** and **`agent_sub_utils.cppm:3032`**: ten
  `// migrated edge case:` blocks, each recording a TS behaviour that would otherwise be lost. All ten
  are transcribed in A.11 — this entry exists so a reader hunting "where did we note TS edges?" has the
  complete list.

- **`cpp_migration/src/ui/app.cppm:417-422`**, **`:455-459`**, **`:655-662`**, **`:666-670`** —
  `TS PARITY FIX (2026-07-04 / 2026-07-05)` blocks. See B.8.

- **`cpp_migration/src/ui/app.cppm:496-502`** — `content_preview IS the rendered message body, not a
  "preview". Do NOT truncate here.` See B.8.

- **`cpp_migration/src/ui/app.cppm:767-769`** — The `!`-command worker thread and why it is separate.
  See B.8.

- **`cpp_migration/src/ui/app.cppm:1232-1243`** and **`ui/app_extra_methods.cpp:80-92`** — The local `!`
  command protocol (`never an LLM turn`, `shouldQuery:false`), duplicated in the TU it was moved to.

- **`cpp_migration/src/ui/app_handle_submit.cpp:302-306`** — `P2 gap stashed-prompt: stash current input
  before compact so the user's typed text survives the context compression.` See B.8.

- **`cpp_migration/src/ui/messages/message_pipeline.cppm:7`**, **`:204-214`**, **`:569-614`**,
  **`:100-106`**, **`:633-635`** — The pipeline's `TS REFERENCE (port location + intent)` header, the
  `PRINCIPLE` block, the numbered visibility rules, the `DedupTracker` invariant, and the `filter_fn`
  simplification. See A.8 / B.2.

- **`cpp_migration/src/ui/messages/message_row.cppm:366`** — **(new)** Dispatch returns a `Component`
  tree ready for insertion into the screen — the result type is part of the contract.

- **`cpp_migration/src/ui/messages/virtual_message_list.cppm:78-85`**, **`:206`**, **`:312`**,
  **`:740`**, **`:877-882`**, **`:964`** — The BMI-cascade rationale; the `height_measured` exact-vs-
  estimate distinction; search returning a **visual_line** not a row index; the diagnostic row-renderer
  fallback; the double-count cap; the C++-only no-op pre-warm. See C.1 / B.8.

- **`cpp_migration/src/ui/tools/tool_ui_longtail.cppm:126`**, **`:157`**, **`:187`**, **`:211`** —
  Visible-vs-hidden classification for the long-tail tools (output **IS** visible for ComputerUse,
  WebBrowser, AskUserQuestion).

- **`cpp_migration/src/skills/bundled.cppm:17`**, **`:25`**, **`:31`** — The root-level
  `keybindings.cppm` / `skillify.cppm` / `update_config.cppm` are **simplified stubs**; the full
  versions are under `bundled/`. See A.12.

- **`cpp_migration/src/skills/lorem_ipsum.cppm:16-17`** and **`:344-345`** — `Preserved exactly
  (canonical opening would change the token-count contract)`; `TS: silently does nothing … we keep
  parity but surface a short hint`. See A.12.

- **`cpp_migration/src/skills/verify_content.cppm:13-15`** — `UI components … are intentionally omitted
  per the Phase 2 scope`. See A.12.

- **`cpp_migration/src/skills/bundled/skill_keybindings.cppm:451`** — `skill name is
  "keybindings-help" (NOT "keybindings")`. See A.12.

- **`cpp_migration/src/skills/load_skills_dir.cppm:373`** — `TS REF: checks pluginOnlyPolicy setting.
  For CPP, we check an env var.` See A.12.

- **`cpp_migration/src/tools/feature_flags.cppm`** (15 marked sites) — Each records the **TS feature
  gate** behind a C++ flag (`KAIROS || KAIROS_PUSH_NOTIFICATION`, `USER_TYPE === 'ant'`,
  `NODE_ENV === 'test'`, `ENABLE_LSP_TOOL`, `isWorktreeModeEnabled`, `isAgentSwarmsEnabled`,
  `hasEmbeddedSearchTools`, `AGENT_TRIGGERS`, `LOOM_VERIFY_PLAN`, `KAIROS_GITHUB_WEBHOOKS`). See A.11.

- **`cpp_migration/src/tools/runtime_registry.cppm:1075-1084`**, **`:2185-2189`** — C++ registers
  TS-gated tools unconditionally and stubs the rest. See A.11.

- **`cpp_migration/src/tools/script_primitives.cppm:509-510`**, **`:610-613`** —
  `deliberately avoid any subprocess work to match the TS contract`.

- **`cpp_migration/src/tools/path_validation.cppm:107-112`**, **`:1037-1038`**,
  **`tools/file_edit_tool.cppm:511-513`**, **`tools/bash_result_formatting.cppm:349-352`**,
  **`tools/sed_edit_parser.cppm:274-275`**, **`tools/agent_memory_snapshot.cppm:76-79`**,
  **`tools/mcp_tool.cppm:1055-1059`** — The `deliberately` / `intentionally` markers across the tools
  layer. See A.11.

- **`cpp_migration/src/utils/file_edit_utils.cppm:310-316`** and **`:1076-1078`** — `We deliberately
  avoid shelling out to diff -u here because …` and the UTF-8/LF simplification. See A.7.

- **`cpp_migration/src/utils/swarm_helpers.cppm:524-527`** — The older TS directory protocol is
  `deliberately not ported`. See B.5.

- **`cpp_migration/src/utils/cron_scheduler.cppm:17`** and **`utils/model/validate_model.cppm:14`** —
  `simplified here to avoid cross-module deps at build time` / `avoids cross-module dependency`.

- **`cpp_migration/src/services/plugins/cli_commands.cppm:72-74`** and
  **`services/plugins/installation_manager.cppm:13-16`** — Superseded shims that **do not fake a
  success string** and are **intentionally not wired to a transport**. See B.7.

- **`cpp_migration/src/services/lsp/LSPServerManager.cppm:831-837`** — The observability-only
  `publishDiagnostics` rule. See B.4.

- **`cpp_migration/src/services/mcp/transport_stdio.cppm:18-21`**,
  **`services/auth/gcp_adc.cppm:27-29`**, **`services/auth/azure_credential.cppm:7`**,
  **`services/auth/sigv4.cppm:461-465`** — `intentionally independent of …` / `intentionally do NOT pull
  in …`. See A.5 / B.3.

- **`cpp_migration/src/ui/markdown.cppm`** (six `The prior divergent renderer …` blocks at lines 1081,
  1103, 1251, 1341, 1376, 1506) — Each names what the *previous C++* did wrong relative to TS and the
  concrete symptom it caused. See A.7. **This is the densest collection of "here is the C++ bug we
  fixed and how to avoid re-introducing it" in the tree.**

- **`cpp_migration/src/ui/messages/messages_list.cppm:3464`** / **`:1819`** — The `N new messages`
  divider is inserted **BEFORE** the row. See C.3.

- **`cpp_migration/src/ui/messages/messages_list.cppm:3592-3601`** — The unseen-divider hash includes
  both a presence bit and the count, and deliberately excludes `streaming_tail_row`. See C.3.

### D.2 — Scattered local definitions of "stale" (no shared contract)

A recurring pattern worth flagging: eleven modules each define *stale* locally, with no shared
definition. Changing the notion in one does not change the others — and none of them can be validated
against TS after deletion.

- `cpp_migration/src/utils/lockfile.cppm:135` — stale lock, older than `max_age`
- `cpp_migration/src/utils/native_installer.cppm:228` — stale lock where the process is no longer running
- `cpp_migration/src/utils/cron_tasks.cppm:44` — stale lock cleanup, every hour
- `cpp_migration/src/utils/swarm_coordination.cppm:273` — stale heartbeat
- `cpp_migration/src/utils/worktree_utils.cppm:113` — stale worktree references (pruned)
- `cpp_migration/src/memdir/memory.cppm:327` — expired / stale memories (removed)
- `cpp_migration/src/services/remote_settings/sync_cache.cppm:33` — cache older than the TTL
- `cpp_migration/src/daemon/worker_registry.cppm:116` — default TTL for `expire_stale()`
- `cpp_migration/src/utils/settings_manager.cppm:522` — re-read bypassing cache "to avoid stale state"
- `cpp_migration/src/ui/dialogs/wizard_dialog.cppm:581` — clear any stale error on the step we returned to
- `cpp_migration/src/cli/handlers/agents.cppm:118` and **`tools/agent_runtime.cppm:3907`** — stale PID
  file removed; stale-worktree reaper mtime bump (the latter is transcribed in A.11)

### D.3 — Intra-C++ line references (same fragility, not covered by the `TS REF` convention)

The `TS REF:` convention did not apply to line references into *other C++ files*, so these are
unchecked and will drift silently. Worth knowing they exist:

- `cpp_migration/src/ui/app_autocomplete.cpp:1242` — references `text_input.cppm L586`
- `cpp_migration/src/ui/messages/message_image.cppm:223` — references `repl_screen.cppm L825-831`
- `cpp_migration/src/ui/messages/messages_list.cppm:2363` — references the divergent path "below" in the
  same file by description, not line
- `cpp_migration/src/ui/dialogs/cost_threshold_dialog.cppm:24` — references an inline lambda in
  `repl_screen.cppm`
- `cpp_migration/src/ui/messages/user_message.cppm:12` — references `RenderUserPromptMessage` in a
  sibling module by name
- `cpp_migration/src/tools/runtime_shared_utils.cppm:13-20` — a list of historical `L###`s recording
  where each helper used to live before the file was split
- `cpp_migration/src/ui/messages/messages_list.cppm:3697` — a `REPL integration note` describing the
  intended call shape for `repl_screen.cppm`'s `RenderMessages` / `MakeReplScreen`, which no longer
  matches the current call site
- `cpp_migration/src/ui/messages/virtual_message_list.cppm:421`, **`:645`**, **`:978`** — the only
  places supplying filenames (`VirtualMessageList.tsx`, `REPL.tsx`) for the bare `L###` references in
  that file; see C.2

---

## Appendix — files containing self-documenting `TS REFERENCE:` blocks

These 25 files (34 blocks total) carry a `TS REFERENCE:` header that already contains a FAITHFUL
FEATURES / KEY BEHAVIOR list. They **self-document** — the TS-side intent is written out in place — so
they are listed here rather than transcribed. If the TS tree's deletion raises a question about one of
these modules, the answer is almost certainly in its own header.

```
cpp_migration/src/ui/dialogs/about_dialog.cppm
cpp_migration/src/ui/dialogs/confirmation_dialog.cppm
cpp_migration/src/ui/dialogs/dialog_frame.cppm
cpp_migration/src/ui/dialogs/dialog_launchers.cppm          (4 blocks)
cpp_migration/src/ui/dialogs/help_view.cppm
cpp_migration/src/ui/dialogs/quick_open.cppm
cpp_migration/src/ui/dialogs/settings_view.cppm
cpp_migration/src/ui/permissions/permission_bash.cppm
cpp_migration/src/ui/prompt/prompt_input_footer.cppm        (7 blocks)
cpp_migration/src/ui/tools/tool_ui_agent.cppm
cpp_migration/src/ui/tools/tool_ui_bash.cppm
cpp_migration/src/ui/tools/tool_ui_file_edit.cppm
cpp_migration/src/ui/tools/tool_ui_file_read.cppm
cpp_migration/src/ui/tools/tool_ui_file_write.cppm
cpp_migration/src/ui/tools/tool_ui_generic.cppm
cpp_migration/src/ui/tools/tool_ui_glob.cppm
cpp_migration/src/ui/tools/tool_ui_grep.cppm
cpp_migration/src/ui/tools/tool_ui_lsp.cppm
cpp_migration/src/ui/tools/tool_ui_mcp.cppm
cpp_migration/src/ui/tools/tool_ui_registry.cppm
cpp_migration/src/ui/tools/tool_ui_skill.cppm
cpp_migration/src/ui/tools/tool_ui_task.cppm
cpp_migration/src/ui/tools/tool_ui_web_fetch.cppm
cpp_migration/src/ui/tools/tool_ui_web_search.cppm
cpp_migration/src/utils/statusline_runner.cppm
```

Three additional files carry a **`FAITHFUL FEATURES`** / **`KEY BEHAVIOR`** list without the
`TS REFERENCE:` label. Same rationale — self-documenting, listed not transcribed:

```
cpp_migration/src/ui/permissions/permission_file_edit.cppm   (FAITHFUL FEATURES, 1:1 with TS)
cpp_migration/src/ui/permissions/permission_file_write.cppm  (FAITHFUL FEATURES, 1:1 with TS)
cpp_migration/src/utils/statusline_runner.cppm               (KEY BEHAVIOR, matching TS)
```

Note: `tool_ui_registry.cppm` and `prompt_input_footer.cppm` appear in **both** the appendix (for their
self-documenting headers) and the main body (for specific decisions that are *not* in those headers —
see D.1 and A.10). Their headers are self-sufficient; the body entries are extra.

---

## Method note

Candidate generation: `grep -rn "TS REF" cpp_migration/src` (1399 hits) plus marker-vocabulary greps
(`deliberately`, `intentional`, `divergence`, `replaces the previous`, `must not`, `cannot`,
`NOTE:`, `IMPORTANT:`, `CROSS-MODULE`, `contract`, `silent`, `coupling`, `workaround`, `because`,
`TS vs CPP`, `DIFFERENCE:`, `fabricated`, `migrated edge case`, `SLOT FALLBACK`, `TS PARITY FIX`),
yielding ~1480 raw candidate comment lines. Pure file/line pointers and ordinary validation strings
were filtered out mechanically; every remaining candidate was then read with 10-20 lines of
surrounding context before being kept or dropped. Findings were rewritten to stand alone: a bullet is
only included if it is intelligible without the deleted TS file.
