/// @file tree_sitter.cppm
/// @brief Lightweight C++23 wrapper around the tree-sitter C API.
///
/// Exports typed RAII handles for TSParser / TSTree / TSQuery / TSQueryCursor
/// plus a Query helper that compiles a pattern and streams capture results.
/// All tree-sitter usage is guarded by CC_HAS_TREE_SITTER. When the dependency is
/// disabled (CC_HAS_TREE_SITTER=0), Parser::parse returns nullptr and Query
/// reports a compilation error so higher layers can fall back conservatively.

module;

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#if CC_HAS_TREE_SITTER
extern "C" {
#include <tree_sitter/api.h>
}
extern "C" const TSLanguage* tree_sitter_bash();
#endif

export module cc.utils.tree_sitter.base;

export namespace cc::utils::tree_sitter {

#if CC_HAS_TREE_SITTER

// ─── RAII deleters for tree-sitter opaque pointers ────────────────────────────────
struct TSLanguageDeleter {
    void operator()(const TSLanguage*) const noexcept {
        // Languages are static data; tree-sitter-bash exposes a singleton that must
        // not be freed. No-op here to keep the unique_ptr signature uniform.
    }
};
using TSLanguageHandle = std::unique_ptr<const TSLanguage, TSLanguageDeleter>;

struct TSTreeDeleter {
    void operator()(TSTree* t) const noexcept {
        if (t) ts_tree_delete(t);
    }
};
using TSTreePtr = std::unique_ptr<TSTree, TSTreeDeleter>;

struct TSParserDeleter {
    void operator()(TSParser* p) const noexcept {
        if (p) ts_parser_delete(p);
    }
};
using TSParserPtr = std::unique_ptr<TSParser, TSParserDeleter>;

struct TSQueryDeleter {
    void operator()(TSQuery* q) const noexcept {
        if (q) ts_query_delete(q);
    }
};
using TSQueryPtr = std::unique_ptr<TSQuery, TSQueryDeleter>;

struct TSQueryCursorDeleter {
    void operator()(TSQueryCursor* c) const noexcept {
        if (c) ts_query_cursor_delete(c);
    }
};
using TSQueryCursorPtr = std::unique_ptr<TSQueryCursor, TSQueryCursorDeleter>;

// ─── Parser ──────────────────────────────────────────────────────────────────
/// Opaque RAII wrapper around TSParser.  Non-copyable, moveable.
class Parser {
public:
    Parser() : p_(ts_parser_new()) {}
    ~Parser() = default;
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;
    Parser(Parser&&) noexcept = default;
    Parser& operator=(Parser&&) noexcept = default;

    /// Bind the parser to a language grammar (e.g. tree_sitter_bash_lang()).
    [[nodiscard]] bool set_language(const TSLanguage* lang) noexcept {
        if (!p_ || !lang) return false;
        return ts_parser_set_language(p_.get(), lang);
    }

    /// Parse a UTF-8 script into a tree.  Returns nullptr on allocation failure.
    [[nodiscard]] TSTreePtr parse(std::string_view src) const noexcept {
        if (!p_) return nullptr;
        return TSTreePtr{ts_parser_parse_string(
            p_.get(), nullptr, src.data(),
            static_cast<uint32_t>(src.size()))};
    }

    /// Access to the raw parser (used for advanced consumers such as re-parsing with
    /// an existing old tree.  Intentionally not exposed in the disabled branch
    /// to avoid leaking internal state.
    [[nodiscard]] TSParser* raw() const noexcept { return p_.get(); }

private:
    TSParserPtr p_;
};

// ─── Query ────────────────────────────────────────────────────────────────────────
/// Compiled tree-sitter query with capture-name cache and match iteration.
class Query {
public:
    struct Capture {
        uint32_t id = 0;
        std::string name;
        uint32_t start_byte = 0;
        uint32_t end_byte = 0;
    };
    struct Match {
        uint32_t query_id = 0;
        uint32_t pattern_index = 0;
        std::vector<Capture> captures;
    };

    /// Compile a S-expression query pattern against the target language.
    /// If compilation fails, ok() returns false and error() describes the issue.
    Query(const TSLanguage* lang, std::string_view pattern) {
        if (!lang) {
            error_msg_ = "null language";
            return;
        }
        uint32_t err_offset = 0;
        TSQueryError err = TSQueryErrorNone;
        q_.reset(ts_query_new(
            lang,
            pattern.data(),
            static_cast<uint32_t>(pattern.size()),
            &err_offset,
            &err));
        if (q_) {
            uint32_t count = ts_query_capture_count(q_.get());
            capture_names_.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t len = 0;
                const char* s = ts_query_capture_name_for_id(q_.get(), i, &len);
                capture_names_[i] = std::string(s, len);
            }
            pattern_count_ = ts_query_pattern_count(q_.get());
        } else {
            error_msg_ = describe_query_error(err, err_offset);
        }
    }

    [[nodiscard]] bool ok() const noexcept { return q_ != nullptr; }
    [[nodiscard]] const std::string& error() const noexcept { return error_msg_; }
    [[nodiscard]] uint32_t pattern_count() const noexcept { return pattern_count_; }

    /// Run the query over `tree` and return all matches ordered by pattern.
    /// The `src` text is optional here because tree-sitter already owns node ranges; the
    /// parameter is retained for symmetry with higher-level text extraction helpers.
    [[nodiscard]] std::vector<Match> matches(TSTree* tree,
                                         std::string_view /*src*/ = {}) const {
        std::vector<Match> out;
        if (!q_ || !tree) return out;
        TSQueryCursorPtr cursor{ts_query_cursor_new()};
        ts_query_cursor_exec(cursor.get(), q_.get(), ts_tree_root_node(tree));
        TSQueryMatch match{};
        while (ts_query_cursor_next_match(cursor.get(), &match)) {
            Match m;
            m.query_id = match.id;
            m.pattern_index = match.pattern_index;
            m.captures.reserve(match.capture_count);
            for (uint16_t i = 0; i < match.capture_count; ++i) {
                const auto& c = match.captures[i];
                Capture cap;
                cap.id = c.index;
                if (c.index < capture_names_.size()) {
                    cap.name = capture_names_[c.index];
                }
                TSNode n = c.node;
                cap.start_byte = ts_node_start_byte(n);
                cap.end_byte   = ts_node_end_byte(n);
                m.captures.push_back(std::move(cap));
            }
            out.push_back(std::move(m));
        }
        return out;
    }

private:
    static std::string describe_query_error(TSQueryError err, uint32_t off) {
        std::string msg = "ts_query_new failed at offset " + std::to_string(off) + ": ";
        switch (err) {
            case TSQueryErrorSyntax:    return msg + "syntax";
            case TSQueryErrorNodeType:  return msg + "unknown node type";
            case TSQueryErrorField:   return msg + "unknown field";
            case TSQueryErrorCapture: return msg + "bad capture";
            case TSQueryErrorStructure: return msg + "bad structure";
            case TSQueryErrorLanguage:  return msg + "language version mismatch";
            default: return msg + "unknown";
        }
    }

    TSQueryPtr q_;
    uint32_t pattern_count_ = 0;
    std::vector<std::string> capture_names_;
    std::string error_msg_;
};

/// Access to the bash language singleton.  The caller keeps a non-owning pointer that
/// remains valid for the lifetime of the program.
[[nodiscard]] inline auto tree_sitter_bash_lang() noexcept -> const TSLanguage* {
    return tree_sitter_bash();
}

#else   // !CC_HAS_TREE_SITTER

// ─── Disabled stubs ────────────────────────────────────────────────────────
// When tree-sitter is not linked we provide zero-dependency stub types so
// callers can still be compiled.  Parser::parse always returns nullptr and
// Query::ok is always false.  Higher layers must treat this as a parse
// failure (conservative fallback → manual approval required).

struct TSLanguageDeleter { void operator()(void const*) const noexcept {} };
using TSLanguageHandle = std::unique_ptr<const void, TSLanguageDeleter>;

struct TSTreeDeleter { void operator()(void*) const noexcept {} };
using TSTreePtr = std::unique_ptr<void, TSTreeDeleter>;

class Parser {
public:
    Parser() noexcept = default;
    ~Parser() = default;
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;
    Parser(Parser&&) noexcept = default;
    Parser& operator=(Parser&&) noexcept = default;

    [[nodiscard]] bool set_language(const void*) const noexcept { return false; }
    [[nodiscard]] TSTreePtr parse(std::string_view /*src*/) const noexcept { return nullptr; }
    [[nodiscard]] void* raw() const noexcept { return nullptr; }
};

class Query {
public:
    struct Capture {
        uint32_t id = 0;
        std::string name;
        uint32_t start_byte = 0;
        uint32_t end_byte = 0;
    };
    struct Match {
        uint32_t query_id = 0;
        uint32_t pattern_index = 0;
        std::vector<Capture> captures;
    };

    Query(const void* /*lang*/, std::string_view /*pattern*/) {
        error_msg_ = "tree-sitter disabled at build time (CC_HAS_TREE_SITTER=0)";
    }

    [[nodiscard]] bool ok() const noexcept { return false; }
    [[nodiscard]] const std::string& error() const noexcept { return error_msg_; }
    [[nodiscard]] uint32_t pattern_count() const noexcept { return 0; }

    [[nodiscard]] std::vector<Match> matches(void* /*tree*/,
                                          std::string_view = {}) const {
        return {};
    }

private:
    std::string error_msg_;
};

[[nodiscard]] inline auto tree_sitter_bash_lang() noexcept -> const void* {
    return nullptr;
}

#endif  // CC_HAS_TREE_SITTER

}  // namespace cc::utils::tree_sitter
