# Error-Handling Conventions

> Status: documented 2026-06-15 (audit §13 #10). This records the canonical
> pattern the codebase already converges on; new code should follow it, and
> legacy modules should be migrated opportunistically.

## Canonical pattern

Use **`std::expected<T, cc::utils::Error>`** (aliased as `cc::utils::Result<T>`)
for any fallible operation that can fail in a recoverable, expected way.

```cpp
[[nodiscard]] cc::utils::Result<AppState> load_state();        // success or Error
[[nodiscard]] std::expected<void, OAuthError> store(...);      // domain-specific error enum is fine
```

- `[[nodiscard]]` on every fallible function — callers must not silently drop
  the error.
- Propagate with `return std::unexpected(err);` / `co_await`-style chaining
  (`if (!r) return std::unexpected(r.error());`).
- Prefer a domain-specific error enum (`OAuthError`, `LspClientError`,
  `cc::utils::ErrorCode`) over raw strings where callers can meaningfully
  branch on the cause. `cc::utils::Error` carries `ErrorCode` + message and is
  the lingua-franca for cross-module boundaries.

## Where each style belongs

| Context | Mechanism | Example |
|---|---|---|
| Fallible service / utility op | `std::expected<T, Error>` | `StatePersistence::load_state` |
| Tool execution result | `ToolResult` (with `is_error` + content) | `BashTool::execute` |
| Truly exceptional / invariant violation / OOM | `throw` (rare) | `yyjson` C-boundary unrecoverable states |
| Recoverable validation inside a reducer | side-effect no-op action + log | `EnableTool`/`SaveState` reducers |

- **`ToolResult::error()`** is the right vehicle *only* for the value a tool
  returns to the model (so the model sees the failure as part of the
  conversation). Do not use it as a general control-flow channel between
  services.
- **Exceptions** are reserved for conditions the caller cannot reasonably
  handle (a broken invariant, a corrupted C-boundary object). Wrap
  exception-throwing C APIs (`yyjson`, `std::stoi`) at the boundary and
  convert to `std::expected` for callers.
- **No-op reducers** (`SaveState`/`LoadState`/`ClearSavedState`/`EnableTool`/
  `DisableTool`) are intentional: their side effects live in the service
  layer, not the reducer. They are documented, not a bug.

## Anti-patterns to avoid

- Returning a sentinel value (`-1`, empty string, `nullptr`) to denote failure
  without a separate status channel.
- `std::expected` at one boundary that gets unwrapped to an exception, then
  re-caught and converted back — collapse to one style per call chain.
- Silently swallowing an `std::expected` error (the `[[nodiscard]]` annotation
  is the first line of defence; reviewers should be the second).

## Migration notes (ongoing)

The codebase is mid-migration: services/state/oauth/lsp already follow this
pattern. `file_edit_utils` was migrated 2026-06-17 — `get_patch_for_edits` /
`get_patch_for_edit` now return `std::expected<PatchForEditsResult, std::string>`
instead of throwing, eliminating the throw-then-catch boundary in
`file_edit_tool` and `are_file_edits_equivalent` (no test asserted the throw,
so the change was safe). The same day `log_error(const std::string&)` was fixed
— it had thrown a `runtime_error` purely to re-enter the exception overload via
catch, a control-flow misuse of exceptions.

A 2026-06-17 audit of the remaining ~33 `throw` sites classified them:

- **Legitimate (kept):** the `json_read` parser throws and is wrapped to
  `std::expected` at the `cc.utils.json` boundary (callers use `parse_json_file`
  which returns `Result`); `stop_task` / `bridge` domain errors are caught by
  their own poll loops; `SanitizedValue::at` mirrors `std::map::at`;
  `words::random_index` throws on a violated precondition.
- **Factory-migrated 2026-06-17:** `CurlHandle` (was a throwing RAII ctor — now
  `create()` returns `Result`, so `post()` / `stream()` keep their `Result`
  chain consistent and the streaming worker thread cannot terminate on init
  failure).
- **Needs investigation:** `bridge_main::run_headless` returns `VoidResult` but
  throws `BridgeHeadlessPermanentError` (no caller catches it; the call chain is
  unclear). Migrate when the bridge module is next touched.
- **Dead code (no external callers — removed 2026-06-17):** `lazy_schema`
  (fully unused module — dropped `get` / `validate_or_throw`, 2 throw sites);
  `keybindings::resolver()` overloads (`m_resolver` is accessed directly in
  `handle_event`, so the accessors were unused).

Migrate when touching a module; do not mass-rewrite without per-module test
coverage.
