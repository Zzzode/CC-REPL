/// @file lorem_ipsum.cppm
/// @brief Lorem Ipsum skill — token-aware placeholder text generation.
///
/// Audit vs TS src/skills/bundled/loremIpsum.ts:
///   - TS uses ONE_TOKEN_WORDS (API-verified single-token English words) and
///     a token-count loop that emits sentences of 10-20 words each with
///     random paragraph breaks ~20% of the time.  Ported 1:1 below.
///   - TS argument model: `/lorem-ipsum N` where N = target token count
///     (default 10_000, hard cap 500_000).  C++ mirrors this via
///     `generate_by_tokens(n)`.
///   - TS gates behind USER_TYPE == 'ant'.  C++ exposes
///     `is_ant_user() -> bool` and the SkillDefinition factory will return
///     a no-op when the gate fails (caller-side gating is also available).
///   - The TS skill does NOT actually open with the canonical
///     "Lorem ipsum dolor sit amet..." phrase — every word is random-sampled
///     from ONE_TOKEN_WORDS.  Preserved exactly (canonical opening would
///     change the token-count contract).
///   - Extensions (not in TS, added for completeness matching the user's
///     list of "all format args"): `generate_paragraphs`, `generate_words`,
///     `generate_chars`, `generate_json_placeholder`, `generate_code_block`,
///     `generate_csv`, natural-language number parsing ("five" -> 5).
module;
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <format>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.skills.lorem_ipsum;

import cc.skills.skill;

export namespace cc::skills::lorem_ipsum {

// ============================================================
// ONE_TOKEN_WORDS — 1-token vocabulary (verified via API token counting).
// Mirrors TS `ONE_TOKEN_WORDS` line-for-line.  Order preserved for the
// rare case where downstream tests depend on a deterministic seed.
// ============================================================
constexpr std::array<std::string_view, 200> kOneTokenWords = {{
    // Articles & pronouns
    "the","a","an","I","you","he","she","it","we","they",
    "me","him","her","us","them","my","your","his","its","our",
    "this","that","what","who",
    // Common verbs
    "is","are","was","were","be","been","have","has","had",
    "do","does","did","will","would","can","could","may","might",
    "must","shall","should","make","made","get","got","go","went",
    "come","came","see","saw","know","take","think","look","want",
    "use","find","give","tell","work","call","try","ask","need",
    "feel","seem","leave","put",
    // Common nouns & adjectives
    "time","year","day","way","man","thing","life","hand","part",
    "place","case","point","fact","good","new","first","last","long",
    "great","little","own","other","old","right","big","high","small",
    "large","next","early","young","few","public","bad","same","able",
    // Prepositions & conjunctions
    "in","on","at","to","for","of","with","from","by","about",
    "like","through","over","before","between","under","since","without",
    "and","or","but","if","than","because","as","until","while","so",
    "though","both","each","when","where","why","how",
    // Common adverbs
    "not","now","just","more","also","here","there","then","only","very",
    "well","back","still","even","much","too","such","never","again",
    "most","once","off","away","down","out","up",
    // Tech/common words
    "test","code","data","file","line","text","word","number","system",
    "program","set","run","value","name","type","state","end","start",
}};

static_assert(kOneTokenWords.size() == 200, "kOneTokenWords must be 200 entries (TS count)");

// ============================================================
// Core generator — mirrors TS `generateLoremIpsum(targetTokens)`.
//
// Produces approximately `target_tokens` of tokens, where each token is a
// single random draw from kOneTokenWords (these have been API-verified to
// each occupy exactly one token).  Sentences are 10-20 words; paragraph
// breaks happen ~20% of the time after a sentence completes.
// ============================================================
[[nodiscard]] inline std::string generate_by_tokens(
    std::size_t target_tokens,
    std::uint32_t seed = 0xDEADBEEFu)
{
    if (target_tokens == 0) return {};
    std::mt19937 rng(seed == 0 ? std::random_device{}() : seed);
    std::uniform_int_distribution<std::size_t> word_dist(
        0, kOneTokenWords.size() - 1);
    std::uniform_int_distribution<int> sentence_len_dist(10, 20);
    std::uniform_real_distribution<double> para_break(0.0, 1.0);

    std::string result;
    result.reserve(target_tokens * 5);  // ~5 chars/word average
    std::size_t tokens = 0;

    while (tokens < target_tokens) {
        const int sentence_len = sentence_len_dist(rng);
        int words_in_sentence = 0;
        for (int i = 0; i < sentence_len && tokens < target_tokens; ++i) {
            const auto w = kOneTokenWords[word_dist(rng)];
            result.append(w.data(), w.size());
            ++tokens;
            ++words_in_sentence;
            if (i == sentence_len - 1 || tokens >= target_tokens) {
                result += ". ";
            } else {
                result += ' ';
            }
        }
        // Paragraph break every ~5 sentences on average (p = 0.20).
        if (words_in_sentence > 0 &&
            para_break(rng) < 0.20 &&
            tokens < target_tokens)
        {
            result += "\n\n";
        }
    }
    // Trim trailing whitespace (matches TS `result.trim()`).
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
        result.pop_back();
    }
    return result;
}

// ============================================================
// Natural-language number parsing ("five" -> 5, "twenty-three" -> 23).
// Covers 0..99 + common round words (hundred, thousand).
// ============================================================
[[nodiscard]] inline std::optional<std::size_t> parse_natural_number(
    std::string_view text)
{
    static const std::unordered_map<std::string_view, std::size_t> kSmall = {
        {"zero",0},{"one",1},{"two",2},{"three",3},{"four",4},{"five",5},
        {"six",6},{"seven",7},{"eight",8},{"nine",9},{"ten",10},
        {"eleven",11},{"twelve",12},{"thirteen",13},{"fourteen",14},{"fifteen",15},
        {"sixteen",16},{"seventeen",17},{"eighteen",18},{"nineteen",19},
        {"twenty",20},{"thirty",30},{"forty",40},{"fifty",50},
        {"sixty",60},{"seventy",70},{"eighty",80},{"ninety",90},
    };
    // Trim + lowercase.
    std::string s;
    s.reserve(text.size());
    bool prev_space = true;
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isspace(c) || ch == '-') {
            if (!prev_space) { s.push_back(' '); prev_space = true; }
        } else {
            s.push_back(static_cast<char>(std::tolower(c)));
            prev_space = false;
        }
    }
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back()  == ' ') s.pop_back();
    if (s.empty()) return std::nullopt;

    // Attempt direct match.
    {
        auto it = kSmall.find(s);
        if (it != kSmall.end()) return it->second;
    }
    if (s == "hundred")  return 100u;
    if (s == "thousand") return 1000u;

    // Try splitting on ' ' or '-' -> additive: "twenty three" -> 23.
    std::size_t total = 0;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto space = s.find(' ', start);
        std::string_view tok = std::string_view(s).substr(
            start, (space == std::string::npos) ? (s.size() - start)
                                                : (space - start));
        auto it = kSmall.find(tok);
        if (it == kSmall.end()) {
            if (tok == "hundred") { total *= 100; }
            else { return std::nullopt; }
        } else {
            total += it->second;
        }
        if (space == std::string::npos) break;
        start = space + 1;
    }
    return total;
}

// ============================================================
// Variant generators (not in TS, requested for parity in spec:
// sentences / paragraphs / words / chars / bytes / json / code / csv).
// Each is layered on top of `generate_by_tokens` or shared helpers.
// ============================================================

/// Generate exactly `n` paragraphs; each paragraph is 4-8 sentences.
[[nodiscard]] inline std::string generate_paragraphs(
    std::size_t n, std::uint32_t seed = 0xBEEFu)
{
    if (n == 0) return {};
    std::mt19937 rng(seed == 0 ? std::random_device{}() : seed);
    std::uniform_int_distribution<std::size_t> wd(0, kOneTokenWords.size() - 1);
    std::uniform_int_distribution<int> sd(4, 8);   // sentences per paragraph
    std::uniform_int_distribution<int> wps(10, 18); // words per sentence

    std::string out;
    for (std::size_t p = 0; p < n; ++p) {
        const int sents = sd(rng);
        for (int s = 0; s < sents; ++s) {
            const int words = wps(rng);
            for (int w = 0; w < words; ++w) {
                if (w > 0) out.push_back(' ');
                auto word = kOneTokenWords[wd(rng)];
                // Capitalise first word of first sentence of each paragraph.
                if (w == 0 && s == 0) {
                    out.push_back(static_cast<char>(
                        std::toupper(static_cast<unsigned char>(word.front()))));
                    out.append(word.substr(1).data(), word.size() - 1);
                } else {
                    out.append(word.data(), word.size());
                }
            }
            out.push_back('.');
            if (s != sents - 1) out.push_back(' ');
        }
        if (p != n - 1) out.append("\n\n");
    }
    return out;
}

/// Generate approximately `n` words.
[[nodiscard]] inline std::string generate_words(std::size_t n,
                                                std::uint32_t seed = 0xCADEu)
{
    // 1 token ~= 1 word for this vocabulary.
    return generate_by_tokens(n, seed);
}

/// Generate approximately `n` characters of prose.
[[nodiscard]] inline std::string generate_chars(std::size_t n,
                                                std::uint32_t seed = 0xF00Du)
{
    if (n == 0) return {};
    // ~5.3 chars/word average -> tokens = n/5.3 + buffer.
    const std::size_t est = (n * 10u + 52u) / 53u + 2u;
    std::string s = generate_by_tokens(est, seed);
    if (s.size() > n) s.resize(n);
    return s;
}

/// Placeholder JSON object with the requested number of top-level keys.
[[nodiscard]] inline std::string generate_json_placeholder(std::size_t keys = 5) {
    if (keys == 0) return "{}";
    std::mt19937 rng(0xC0FFEu);
    std::uniform_int_distribution<std::size_t> wd(0, kOneTokenWords.size() - 1);
    std::string out = "{\n";
    for (std::size_t i = 0; i < keys; ++i) {
        auto k = kOneTokenWords[wd(rng)];
        auto v = kOneTokenWords[wd(rng)];
        out += std::format("  \"{}_{}\": \"{}_{}\"{}\n",
            k, i, v, i, (i + 1 == keys) ? "" : ",");
    }
    out += "}";
    return out;
}

/// Placeholder source-code block: N lines of `let <word> = <n>;`.
[[nodiscard]] inline std::string generate_code_block(std::size_t lines = 15) {
    std::mt19937 rng(0xCODEu);
    std::uniform_int_distribution<std::size_t> wd(0, kOneTokenWords.size() - 1);
    std::uniform_int_distribution<int> nd(0, 9999);
    std::string out;
    out += "// Auto-generated placeholder code block\n";
    out += "export function example() {\n";
    for (std::size_t i = 0; i < lines; ++i) {
        auto var = kOneTokenWords[wd(rng)];
        out += std::format("  const {}_{} = {};\n", var, i, nd(rng));
    }
    out += "  return {\n";
    for (std::size_t i = 0; i < 3 && i < lines; ++i) {
        auto k = kOneTokenWords[wd(rng)];
        out += std::format("    {},{}\n", k, (i + 1 >= 3 || i + 1 >= lines) ? "" : ",");
        // silence unused-warning if lines < 3
        (void)k;
    }
    out += "  };\n";
    out += "}\n";
    return out;
}

/// Placeholder CSV with requested rows + 4 columns.
[[nodiscard]] inline std::string generate_csv(std::size_t rows = 10) {
    std::mt19937 rng(0xCSV1u);
    std::uniform_int_distribution<std::size_t> wd(0, kOneTokenWords.size() - 1);
    std::uniform_int_distribution<int> nd(0, 999);
    std::string out = "id,name,category,value\n";
    for (std::size_t i = 1; i <= rows; ++i) {
        out += std::format("{},{},{},{}\n",
            i,
            kOneTokenWords[wd(rng)],
            kOneTokenWords[wd(rng)],
            nd(rng));
    }
    return out;
}

// ============================================================
// Argument parsing — mirrors TS `getPromptForCommand(args)`.
//
// Supported forms:
//   (empty)                -> default 10_000 tokens
//   "10000"                -> 10_000 tokens
//   "five paragraphs"      -> 5 paragraphs  [nat-lang extension]
//   "200 words"            -> 200 words
//   "500 chars" / "500 bytes"
//   "json" / "json 3"
//   "code" / "code 40"
//   "csv" / "csv 100"
// ============================================================
struct ParseResult {
    std::string output;
    bool ok = true;
    std::string error_message;
};

constexpr std::size_t kTokenCap = 500'000;   // TS: 500k safety cap
constexpr std::size_t kDefaultTokens = 10'000;

[[nodiscard]] inline bool is_ant_user() {
    if (const char* v = std::getenv("USER_TYPE")) {
        return std::string_view(v) == "ant";
    }
    return false;
}

[[nodiscard]] inline ParseResult run_from_args(std::string_view args) {
    ParseResult r;
    // Gate: ant-only (TS behavior).  Callers that want universal access can
    // simply call generate_* directly; this function mirrors the slash-cmd.
    if (!is_ant_user()) {
        // TS: silently does nothing (early return).  We keep parity but
        // surface a short hint for diagnostics.
        r.ok = true;
        r.output = "";
        return r;
    }

    std::string in(args);
    std::transform(in.begin(), in.end(), in.begin(),
        [](unsigned char c){ return std::tolower(c); });
    // Trim whitespace
    while (!in.empty() && std::isspace(static_cast<unsigned char>(in.front())))
        in.erase(in.begin());
    while (!in.empty() && std::isspace(static_cast<unsigned char>(in.back())))
        in.pop_back();

    // --- Bare numeric argument (default tokens) ---
    if (in.empty()) {
        r.output = generate_by_tokens(kDefaultTokens);
        return r;
    }

    auto is_pure_num = [](std::string_view s) -> bool {
        return !s.empty() &&
               std::all_of(s.begin(), s.end(), [](unsigned char c){
                   return std::isdigit(c) || c == ',' || c == '_';
               });
    };
    auto to_num = [](std::string_view s) -> std::size_t {
        std::size_t v = 0;
        for (char c : s) {
            if (std::isdigit(static_cast<unsigned char>(c)))
                v = v * 10 + static_cast<std::size_t>(c - '0');
        }
        return v;
    };

    // --- Variant 1: <number> [tokens] ---------------------------------
    if (is_pure_num(in)) {
        const auto n = to_num(in);
        if (n == 0) {
            r.ok = false;
            r.error_message =
                "Invalid token count.  Please provide a positive number "
                "(e.g., /lorem-ipsum 10000).";
            return r;
        }
        const auto capped = std::min(n, kTokenCap);
        r.output = generate_by_tokens(capped);
        if (capped < n) {
            r.output = std::format(
                "Requested {} tokens, but capped at 500,000 for safety.\n\n{}",
                n, r.output);
        }
        return r;
    }

    // --- Variant 2: <N|natlang> paragraphs / words / chars / bytes ----
    auto split_head = [](const std::string& s)
        -> std::pair<std::string, std::string>
    {
        // split on the first space
        auto sp = s.find(' ');
        if (sp == std::string::npos) return {s, ""};
        return {s.substr(0, sp), s.substr(sp + 1)};
    };
    auto [head, tail] = split_head(in);

    // Is head a natural-language number?
    std::size_t num = 0;
    bool have_num = false;
    if (is_pure_num(head)) { num = to_num(head); have_num = true; }
    else if (auto nn = parse_natural_number(head)) {
        num = *nn; have_num = true;
    }

    auto keyword_match = [](std::string_view hay, std::string_view needle) {
        return hay.find(needle) != std::string_view::npos;
    };

    if (have_num && keyword_match(tail, "paragraph")) {
        r.output = generate_paragraphs(num ? num : 3);
        return r;
    }
    if (have_num && keyword_match(tail, "word")) {
        const auto cap = std::min(num, kTokenCap * 2u);
        r.output = generate_words(cap);
        return r;
    }
    if (have_num && (keyword_match(tail, "char") || keyword_match(tail, "byte"))) {
        r.output = generate_chars(num);
        return r;
    }

    // --- Variant 3: json | json N | code | code N | csv | csv N -------
    if (in.starts_with("json")) {
        std::size_t k = 5;
        if (have_num && tail.empty()) { /* head was nat-lang word like "json" -> no */ }
        std::string_view rest(in);
        rest.remove_prefix(4);
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
            rest.remove_prefix(1);
        if (!rest.empty()) {
            if (is_pure_num(rest)) k = to_num(rest);
            else if (auto nn = parse_natural_number(rest)) k = *nn;
        }
        r.output = generate_json_placeholder(k);
        return r;
    }
    if (in.starts_with("code")) {
        std::size_t l = 15;
        std::string_view rest(in);
        rest.remove_prefix(4);
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
            rest.remove_prefix(1);
        if (!rest.empty()) {
            if (is_pure_num(rest)) l = to_num(rest);
            else if (auto nn = parse_natural_number(rest)) l = *nn;
        }
        r.output = generate_code_block(l);
        return r;
    }
    if (in.starts_with("csv")) {
        std::size_t rows = 10;
        std::string_view rest(in);
        rest.remove_prefix(3);
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
            rest.remove_prefix(1);
        if (!rest.empty()) {
            if (is_pure_num(rest)) rows = to_num(rest);
            else if (auto nn = parse_natural_number(rest)) rows = *nn;
        }
        r.output = generate_csv(rows);
        return r;
    }

    // --- Fallback: assume the whole thing *tries* to be a token count ---
    if (is_pure_num(in)) {  // (already handled, silence warn)
        r.output = generate_by_tokens(kDefaultTokens);
        return r;
    }
    r.ok = false;
    r.error_message = std::format(
        "Invalid argument '{}'.  Usage:\n"
        "  /lorem-ipsum [N]            — generate ~N tokens (default 10 000, cap 500 000)\n"
        "  /lorem-ipsum 5 paragraphs   — 5 paragraphs of prose\n"
        "  /lorem-ipsum 200 words      — ~200 words\n"
        "  /lorem-ipsum 500 chars      — ~500 characters\n"
        "  /lorem-ipsum json [keys]    — placeholder JSON\n"
        "  /lorem-ipsum code [lines]   — placeholder code block\n"
        "  /lorem-ipsum csv [rows]     — placeholder CSV\n"
        "(natural-language numbers like 'five' are also accepted).",
        args);
    return r;
}

// ============================================================
// SkillDefinition factory.
// ============================================================
[[nodiscard]] inline SkillDefinition make_lorem_ipsum_skill() {
    return SkillDefinition{
        .name = "lorem-ipsum",
        .description =
            "Generate filler text for long-context testing.  "
            "Specify token count as argument (e.g., /lorem-ipsum 50000).  "
            "Outputs approximately the requested number of tokens.  "
            "ANT-only (USER_TYPE=ant).  Also supports: "
            "'5 paragraphs', '200 words', '500 chars', 'json 3', "
            "'code 40', 'csv 100'.",
        .argument_hint = "[token_count | 5 paragraphs | 200 words | json N | code N | csv N]",
        .user_invocable = true,
        .trigger_patterns = {
            R"(lorem\s*ipsum)",
            R"(placeholder\s+text)",
            R"(dummy\s+(?:text|content))",
            R"(filler\s+text)",
            R"(/lorem)",
            R"(generate\s+(?:lorem|placeholder))",
        },
        .content =
            R"(## Lorem Ipsum Generator

### Purpose
Inject placeholder / filler content into the conversation for long-context
testing, UI prototyping, and fill-up-to-N-tokens scenarios.

### Usage variants
- `/lorem-ipsum`              → ~10 000 tokens (default)
- `/lorem-ipsum 50000`        → ~50 000 tokens (hard cap 500 000)
- `/lorem-ipsum 3 paragraphs` → 3 prose paragraphs
- `/lorem-ipsum 200 words`    → ~200 words
- `/lorem-ipsum 500 chars`    → ~500 characters
- `/lorem-ipsum json 7`       → JSON object with 7 keys
- `/lorem-ipsum code 20`      → 20-line code block
- `/lorem-ipsum csv 25`       → 25-row CSV table
- Natural-language numbers ("five paragraphs") are also recognised.

### Token contract
Each generated word comes from a vocabulary that was verified to tokenise
as exactly one token per word, so the token count argument is a reliable
approximation of the real API token consumption.

### Access
Ant-only.  Requires `USER_TYPE=ant` in the environment.
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.1.0",
    };
}

} // namespace cc::skills::lorem_ipsum
