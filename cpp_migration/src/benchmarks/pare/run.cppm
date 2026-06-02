module;

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <map>
#include <algorithm>
#include <optional>

export module cc.benchmarks.pare.run;

import cc.benchmarks.pare.schema;
import cc.benchmarks.pare.case_loader;
import cc.benchmarks.pare.execute_ref;
import cc.benchmarks.pare.metrics;
import cc.benchmarks.pare.workspace;
import cc.utils.json;
import cc.utils.file;

export namespace cc::benchmarks::pare {

namespace fs = std::filesystem;
namespace json = cc::utils::json;

inline auto case_result_jsonl(const CaseRunResult& r) -> std::string {
    json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("case_id", doc.string(r.case_id));
    root.add("pass", doc.boolean(r.pass));
    doc.set_root(root);
    return doc.to_string();
}

inline auto variant_run_json(const VariantRun& vr, bool pretty = true) -> std::string {
    json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("label", doc.string(vr.label));
    root.add("started_at", doc.string(vr.started_at));
    root.add("ended_at", doc.string(vr.ended_at));
    doc.set_root(root);
    return pretty ? doc.to_pretty_string() : doc.to_string();
}

inline auto comparison_summary_json(const ComparisonResult& comparison) -> std::string {
    json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("version", doc.string(comparison.version));
    root.add("timestamp", doc.string(comparison.timestamp));
    doc.set_root(root);
    return doc.to_pretty_string();
}

inline auto run_meta_json(const std::string& started_at, const std::string& finished_at,
                         const std::string& cases_path, const std::string& hash) -> std::string {
    json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("started_at", doc.string(started_at));
    root.add("finished_at", doc.string(finished_at));
    root.add("cases_path", doc.string(cases_path));
    root.add("case_set_hash", doc.string(hash));
    doc.set_root(root);
    return doc.to_pretty_string();
}

inline AggregateResult aggregate(const VariantRun& run) {
    AggregateResult agg;
    agg.total_count = run.results.size();
    agg.pass_count = 0;
    agg.total_duration_ms = 0;
    agg.error_count = 0;
    
    std::vector<NormalizedUsage> usages;
    std::vector<double> durations;
    std::map<std::string, int64_t> error_counts;
    
    for (const auto& result : run.results) {
        if (result.pass) ++agg.pass_count;
        usages.push_back(result.usage);
        
        if (result.duration_ms) {
            durations.push_back(static_cast<double>(*result.duration_ms));
            agg.total_duration_ms += *result.duration_ms;
        }
        
        if (result.error) {
            ++agg.error_count;
            std::string err = *result.error;
            if (err.size() > 100) err = err.substr(0, 100);
            error_counts[err]++;
        }
    }
    
    agg.pass_rate = agg.total_count > 0 ? static_cast<double>(agg.pass_count) / agg.total_count : 0.0;
    agg.avg_duration_ms = agg.total_count > 0 ? static_cast<double>(agg.total_duration_ms) / agg.total_count : 0.0;
    agg.error_rate = agg.total_count > 0 ? static_cast<double>(agg.error_count) / agg.total_count : 0.0;
    agg.tokens = sum_usage(usages);
    agg.p50_duration_ms = p50(durations);
    
    std::vector<std::pair<std::string, int64_t>> error_vec(error_counts.begin(), error_counts.end());
    std::sort(error_vec.begin(), error_vec.end(),
             [](const auto& a, const auto& b) { return a.second > b.second; });
    if (error_vec.size() > 3) {
        error_vec.resize(3);
    }
    agg.top_errors = error_vec;
    
    return agg;
}

inline GroupedSummary grouped_summary(const VariantRun& run) {
    GroupedSummary summary;
    
    for (const auto& result : run.results) {
        std::string category = "uncategorized";
        std::string use_frequency = "unspecified";
        
        if (result.metadata) {
            if (result.metadata->category) category = *result.metadata->category;
            if (result.metadata->use_frequency) use_frequency = *result.metadata->use_frequency;
        }
        
        auto& cat_item = summary.by_category[category];
        cat_item.total++;
        if (result.pass) cat_item.pass++;
        cat_item.tokens += result.usage.total_tokens;
        
        auto& freq_item = summary.by_frequency[use_frequency];
        freq_item.total++;
        if (result.pass) freq_item.pass++;
        freq_item.tokens += result.usage.total_tokens;
    }
    
    return summary;
}

struct RunBenchmarkResult {
    ComparisonResult comparison;
    std::string artifact_dir;
};

struct RunBenchmarkParams {
    std::string cases_path;
    std::optional<std::string> model;
    std::string permission_mode;
    std::optional<size_t> max_cases;
    size_t runs_per_case = 1;
    VariantConfig baseline;
    VariantConfig candidate;
    bool fail_on_regression = false;
    WorkspaceMode workspace_mode = WorkspaceMode::TmpGit;
};

inline std::optional<RunBenchmarkResult> run_benchmark(const RunBenchmarkParams& params) {
    auto loaded = load_case_set(params.cases_path);
    if (!loaded) {
        return std::nullopt;
    }
    
    auto workspace_opt = create_isolated_workspace(params.workspace_mode, fs::current_path().string());
    if (!workspace_opt) {
        return std::nullopt;
    }
    auto& workspace = *workspace_opt;
    
    std::string run_started_at = get_iso8601();
    std::string stamp = run_started_at;
    for (char& c : stamp) {
        if (c == ':' || c == '.') c = '-';
    }
    
    fs::path artifact_dir = fs::current_path() / ".artifacts" / "benchmarks" / "pare" / stamp;
    try {
        fs::create_directories(artifact_dir);
    } catch (...) {
        workspace.cleanup();
        return std::nullopt;
    }
    
    fs::path baseline_stream_path = artifact_dir / "baseline.results.jsonl";
    fs::path candidate_stream_path = artifact_dir / "candidate.results.jsonl";
    
    std::ofstream baseline_stream(baseline_stream_path);
    std::ofstream candidate_stream(candidate_stream_path);
    
    auto baseline_run = execute_variant(ExecuteVariantParams{
        .label = params.baseline.label,
        .command = params.baseline.command,
        .args = params.baseline.args,
        .model = params.model,
        .permission_mode = params.permission_mode,
        .max_cases = params.max_cases,
        .runs_per_case = params.runs_per_case,
        .cases_path = loaded->absolute_path,
        .case_set_hash = loaded->hash,
        .cases = loaded->case_set.cases,
        .on_case_result = [&](const CaseRunResult& r) {
            baseline_stream << case_result_jsonl(r) << "\n";
        },
        .cwd = workspace.dir
    });
    
    auto candidate_run = execute_variant(ExecuteVariantParams{
        .label = params.candidate.label,
        .command = params.candidate.command,
        .args = params.candidate.args,
        .model = params.model,
        .permission_mode = params.permission_mode,
        .max_cases = params.max_cases,
        .runs_per_case = params.runs_per_case,
        .cases_path = loaded->absolute_path,
        .case_set_hash = loaded->hash,
        .cases = loaded->case_set.cases,
        .on_case_result = [&](const CaseRunResult& r) {
            candidate_stream << case_result_jsonl(r) << "\n";
        },
        .cwd = workspace.dir
    });
    
    baseline_stream.close();
    candidate_stream.close();
    
    auto baseline_agg = aggregate(baseline_run);
    auto candidate_agg = aggregate(candidate_run);
    
    std::map<std::string, const CaseRunResult*> baseline_by_id, candidate_by_id;
    for (const auto& r : baseline_run.results) {
        baseline_by_id[r.case_id] = &r;
    }
    for (const auto& r : candidate_run.results) {
        candidate_by_id[r.case_id] = &r;
    }
    
    ComparisonResult comparison;
    comparison.version = "pare-benchmark-v1";
    comparison.timestamp = get_iso8601();
    comparison.config.cases_path = loaded->absolute_path;
    comparison.config.case_set_hash = loaded->hash;
    comparison.config.model = params.model;
    comparison.config.permission_mode = params.permission_mode;
    comparison.config.baseline = params.baseline;
    comparison.config.candidate = params.candidate;
    comparison.aggregate.baseline = baseline_agg;
    comparison.aggregate.candidate = candidate_agg;
    
    comparison.aggregate.delta.pass_rate = candidate_agg.pass_rate - baseline_agg.pass_rate;
    comparison.aggregate.delta.total_tokens = candidate_agg.tokens.total_tokens - baseline_agg.tokens.total_tokens;
    comparison.aggregate.delta.total_duration_ms = candidate_agg.total_duration_ms - baseline_agg.total_duration_ms;
    comparison.aggregate.delta.avg_duration_ms = candidate_agg.avg_duration_ms - baseline_agg.avg_duration_ms;
    
    if (baseline_agg.tokens.total_tokens > 0) {
        comparison.aggregate.delta.total_tokens_pct = comparison.aggregate.delta.total_tokens / static_cast<double>(baseline_agg.tokens.total_tokens);
    } else {
        comparison.aggregate.delta.total_tokens_pct = 0.0;
    }
    
    if (baseline_agg.total_duration_ms > 0) {
        comparison.aggregate.delta.total_duration_pct = comparison.aggregate.delta.total_duration_ms / static_cast<double>(baseline_agg.total_duration_ms);
    } else {
        comparison.aggregate.delta.total_duration_pct = 0.0;
    }
    
    if (baseline_agg.avg_duration_ms > 0) {
        comparison.aggregate.delta.avg_duration_pct = comparison.aggregate.delta.avg_duration_ms / baseline_agg.avg_duration_ms;
    } else {
        comparison.aggregate.delta.avg_duration_pct = 0.0;
    }
    
    int64_t evaluated_cases = 0, skipped_cases = 0, unavailable_cases = 0;
    int64_t weighted_baseline = 0, weighted_delta = 0;
    
    for (const auto& c : loaded->case_set.cases) {
        auto it_b = baseline_by_id.find(c.id);
        auto it_c = candidate_by_id.find(c.id);
        if (it_b == baseline_by_id.end() || it_c == candidate_by_id.end()) continue;
        
        const auto& base_result = *it_b->second;
        const auto& cand_result = *it_c->second;
        
        auto per_case = PerCaseComparison{};
        per_case.id = c.id;
        per_case.name = c.name;
        per_case.baseline = base_result;
        per_case.candidate = cand_result;
        per_case.delta.pass_changed = base_result.pass != cand_result.pass;
        per_case.delta.total_tokens = cand_result.usage.total_tokens - base_result.usage.total_tokens;
        if (base_result.duration_ms && cand_result.duration_ms) {
            per_case.delta.duration_ms = *cand_result.duration_ms - *base_result.duration_ms;
        }
        comparison.per_case.push_back(per_case);
        
        if (cand_result.status != CaseRunStatus::Unavailable && cand_result.status != CaseRunStatus::Skipped) {
            evaluated_cases++;
            weighted_baseline += base_result.usage.total_tokens;
            weighted_delta += (base_result.usage.total_tokens - cand_result.usage.total_tokens);
        } else if (cand_result.status == CaseRunStatus::Skipped) {
            skipped_cases++;
        } else {
            unavailable_cases++;
        }
    }
    
    comparison.aggregate.v2 = V2AggregateSummary{
        .runs_per_case = static_cast<int64_t>(std::max<size_t>(1, params.runs_per_case)),
        .evaluated_cases = evaluated_cases,
        .skipped_cases = skipped_cases,
        .unavailable_cases = unavailable_cases,
        .weighted_token_reduction_pct = weighted_baseline > 0 ? static_cast<double>(weighted_delta) / weighted_baseline : 0.0
    };
    
    comparison.grouped.baseline = grouped_summary(baseline_run);
    comparison.grouped.candidate = grouped_summary(candidate_run);
    
    auto write_variant_run = [&](const VariantRun& vr, const fs::path& path) {
        std::ofstream f(path);
        f << variant_run_json(vr);
    };
    
    write_variant_run(baseline_run, artifact_dir / "baseline.json");
    write_variant_run(candidate_run, artifact_dir / "candidate.json");
    
    {
        std::ofstream f(artifact_dir / "comparison.json");
        f << comparison_summary_json(comparison);
    }
    
    if (params.fail_on_regression) {
        bool token_regression = comparison.aggregate.delta.total_tokens > 0;
        bool accuracy_regression = comparison.aggregate.delta.pass_rate < 0;
        if (token_regression || accuracy_regression) {
            workspace.cleanup();
            return std::nullopt;
        }
    }
    
    {
        std::ofstream f(artifact_dir / "run.meta.json");
        f << run_meta_json(run_started_at, comparison.timestamp, loaded->absolute_path, loaded->hash);
    }
    
    workspace.cleanup();
    
    return RunBenchmarkResult{
        .comparison = comparison,
        .artifact_dir = artifact_dir.string()
    };
}

} // namespace cc::benchmarks::pare
