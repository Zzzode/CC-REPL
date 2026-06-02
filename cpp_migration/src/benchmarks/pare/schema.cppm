module;

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <variant>
#include <chrono>

export module cc.benchmarks.pare.schema;

export namespace cc::benchmarks::pare {

using NormalizeMode = std::string;

struct AssertionExact {
    std::string expected;
    std::vector<NormalizeMode> normalize;
};

struct AssertionRegex {
    std::string pattern;
    std::optional<std::string> flags;
};

struct AssertionIncludes {
    std::string expected;
    std::vector<NormalizeMode> normalize;
};

using Assertion = std::variant<AssertionExact, AssertionRegex, AssertionIncludes>;

struct CaseMetadata {
    std::optional<std::string> pare_scenario_id;
    std::optional<std::string> pare_scenario_name;
    std::optional<std::string> category;
    std::optional<std::string> use_frequency;
    std::optional<std::string> command;
    std::unordered_map<std::string, std::string> extra;
};

struct PareCase {
    std::string id;
    std::string name;
    std::string prompt;
    Assertion assertion;
    std::vector<std::string> tags;
    std::optional<CaseMetadata> metadata;
};

struct CaseSetSource {
    std::optional<std::string> origin;
    std::optional<std::string> migration_date;
    std::optional<std::string> note;
    std::unordered_map<std::string, std::string> extra;
};

struct PareCaseSet {
    std::string version;
    std::optional<CaseSetSource> source;
    std::vector<PareCase> cases;
};

struct NormalizedUsage {
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cache_read_input_tokens = 0;
    int64_t cache_creation_input_tokens = 0;
    int64_t total_tokens = 0;
};

enum class CaseRunStatus {
    Evaluated,
    Skipped,
    Unavailable
};

struct CaseRunResult;

struct SingleRun {
    bool pass;
    std::optional<std::string> error;
    NormalizedUsage usage;
    std::optional<int64_t> duration_ms;
};

struct CaseRunResult {
    std::string case_id;
    std::string case_name;
    bool pass;
    std::optional<std::string> error;
    std::string assistant_text;
    std::string raw_output;
    NormalizedUsage usage;
    std::optional<int64_t> duration_ms;
    std::optional<CaseMetadata> metadata;
    CaseRunStatus status = CaseRunStatus::Evaluated;
    std::vector<SingleRun> runs;
};

struct VariantRun {
    std::string label;
    std::string command;
    std::vector<std::string> args;
    std::optional<std::string> model;
    std::optional<std::string> permission_mode;
    std::string cases_path;
    std::string case_set_hash;
    std::string started_at;
    std::string ended_at;
    std::vector<CaseRunResult> results;
};

struct GroupedItem {
    int64_t total = 0;
    int64_t pass = 0;
    int64_t tokens = 0;
};

struct GroupedSummary {
    std::unordered_map<std::string, GroupedItem> by_category;
    std::unordered_map<std::string, GroupedItem> by_frequency;
};

struct AggregateResult {
    double pass_rate;
    int64_t pass_count;
    int64_t total_count;
    NormalizedUsage tokens;
    std::optional<double> p50_duration_ms;
    int64_t total_duration_ms;
    double avg_duration_ms;
    int64_t error_count;
    double error_rate;
    std::vector<std::pair<std::string, int64_t>> top_errors;
};

struct VariantConfig {
    std::string label;
    std::string command;
    std::vector<std::string> args;
};

struct AggregateDelta {
    double pass_rate;
    int64_t total_tokens;
    double total_tokens_pct;
    int64_t total_duration_ms;
    double total_duration_pct;
    double avg_duration_ms;
    double avg_duration_pct;
};

struct V2AggregateSummary {
    int64_t runs_per_case;
    int64_t evaluated_cases;
    int64_t skipped_cases;
    int64_t unavailable_cases;
    double weighted_token_reduction_pct;
};

struct CaseDelta {
    bool pass_changed;
    int64_t total_tokens;
    std::optional<int64_t> duration_ms;
};

struct PerCaseComparison {
    std::string id;
    std::string name;
    CaseRunResult baseline;
    CaseRunResult candidate;
    CaseDelta delta;
};

struct ComparisonResult {
    std::string version;
    std::string timestamp;
    struct {
        std::string cases_path;
        std::string case_set_hash;
        std::optional<std::string> model;
        std::string permission_mode;
        VariantConfig baseline;
        VariantConfig candidate;
    } config;
    struct {
        AggregateResult baseline;
        AggregateResult candidate;
        AggregateDelta delta;
        std::optional<V2AggregateSummary> v2;
    } aggregate;
    struct {
        GroupedSummary baseline;
        GroupedSummary candidate;
    } grouped;
    std::vector<PerCaseComparison> per_case;
};

} // namespace cc::benchmarks::pare
