module;

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <optional>
#include <cstdlib>

export module cc.benchmarks.pare.cli;

import cc.benchmarks.pare.schema;
import cc.benchmarks.pare.run;
import cc.benchmarks.pare.case_loader;
import cc.benchmarks.pare.workspace;
import cc.utils.json;

export namespace cc::benchmarks::pare {

namespace fs = std::filesystem;
namespace json = cc::utils::json;

struct ParsedArgs {
    std::optional<std::string> config_path;
    std::string cases = "benchmarks/pare/cases/core.json";
    std::optional<std::string> model;
    std::string permission_mode = "bypassPermissions";
    std::optional<size_t> max_cases;
    bool fail_on_regression = false;
    size_t runs_per_case = 1;
    std::string baseline_label = "baseline";
    std::string baseline_command = "bun";
    std::vector<std::string> baseline_args = {"dist/cli.js"};
    std::string candidate_label = "candidate";
    std::string candidate_command = "bun";
    std::vector<std::string> candidate_args = {"dist/cli.js"};
    WorkspaceMode workspace_mode = WorkspaceMode::TmpGit;
    bool show_help = false;
};

struct BenchmarkConfig {
    std::optional<std::string> cases;
    std::optional<std::string> model;
    std::optional<std::string> permission_mode;
    std::optional<size_t> max_cases;
    std::optional<bool> fail_on_regression;
    std::optional<size_t> runs_per_case;
    std::optional<WorkspaceMode> workspace_mode;
    struct Variant {
        std::optional<std::string> label;
        std::optional<std::string> command;
        std::optional<std::vector<std::string>> args;
    };
    std::optional<Variant> baseline;
    std::optional<Variant> candidate;
};

inline std::optional<BenchmarkConfig> load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    auto doc = json::parse(content);
    if (!doc) {
        return std::nullopt;
    }
    
    auto root = doc->root();
    if (!root || !root.is_obj()) {
        return std::nullopt;
    }
    
    BenchmarkConfig cfg;
    
    auto cases_val = root.get("cases");
    if (cases_val && cases_val.is_str()) {
        cfg.cases = std::string(cases_val.as_str());
    }
    
    auto model_val = root.get("model");
    if (model_val && model_val.is_str()) {
        cfg.model = std::string(model_val.as_str());
    }
    
    auto perm_val = root.get("permission_mode");
    if (perm_val && perm_val.is_str()) {
        cfg.permission_mode = std::string(perm_val.as_str());
    }
    
    auto max_cases_val = root.get("max_cases");
    if (max_cases_val && max_cases_val.is_num()) {
        cfg.max_cases = static_cast<size_t>(max_cases_val.as_int());
    }
    
    auto fail_val = root.get("fail_on_regression");
    if (fail_val && fail_val.is_bool()) {
        cfg.fail_on_regression = fail_val.as_bool();
    }
    
    auto runs_val = root.get("runs_per_case");
    if (runs_val && runs_val.is_num()) {
        cfg.runs_per_case = static_cast<size_t>(runs_val.as_int());
    }
    
    auto ws_val = root.get("workspace_mode");
    if (ws_val && ws_val.is_str()) {
        std::string ws(ws_val.as_str());
        if (ws == "tmp-git") cfg.workspace_mode = WorkspaceMode::TmpGit;
        else if (ws == "worktree") cfg.workspace_mode = WorkspaceMode::Worktree;
        else if (ws == "current") cfg.workspace_mode = WorkspaceMode::Current;
    }
    
    auto parse_variant = [](json::JsonVal v) -> std::optional<BenchmarkConfig::Variant> {
        if (!v || !v.is_obj()) return std::nullopt;
        BenchmarkConfig::Variant var;
        auto label = v.get("label");
        if (label && label.is_str()) var.label = std::string(label.as_str());
        auto cmd = v.get("command");
        if (cmd && cmd.is_str()) var.command = std::string(cmd.as_str());
        auto args = v.get("args");
        if (args && args.is_arr()) {
            std::vector<std::string> arr;
            size_t sz = args.size();
            for (size_t i = 0; i < sz; ++i) {
                auto item = args.at(i);
                if (item && item.is_str()) {
                    arr.push_back(std::string(item.as_str()));
                }
            }
            var.args = arr;
        }
        return var;
    };
    
    auto baseline_val = root.get("baseline");
    cfg.baseline = parse_variant(baseline_val);
    
    auto candidate_val = root.get("candidate");
    cfg.candidate = parse_variant(candidate_val);
    
    return cfg;
}

inline ParsedArgs apply_config(ParsedArgs base, const BenchmarkConfig& cfg) {
    ParsedArgs merged = std::move(base);
    
    if (cfg.cases) merged.cases = *cfg.cases;
    if (cfg.model) merged.model = *cfg.model;
    if (cfg.permission_mode) merged.permission_mode = *cfg.permission_mode;
    if (cfg.max_cases) merged.max_cases = *cfg.max_cases;
    if (cfg.fail_on_regression) merged.fail_on_regression = *cfg.fail_on_regression;
    if (cfg.runs_per_case) merged.runs_per_case = *cfg.runs_per_case;
    if (cfg.workspace_mode) merged.workspace_mode = *cfg.workspace_mode;
    
    if (cfg.baseline) {
        if (cfg.baseline->label) merged.baseline_label = *cfg.baseline->label;
        if (cfg.baseline->command) merged.baseline_command = *cfg.baseline->command;
        if (cfg.baseline->args) merged.baseline_args = *cfg.baseline->args;
    }
    
    if (cfg.candidate) {
        if (cfg.candidate->label) merged.candidate_label = *cfg.candidate->label;
        if (cfg.candidate->command) merged.candidate_command = *cfg.candidate->command;
        if (cfg.candidate->args) merged.candidate_args = *cfg.candidate->args;
    }
    
    return merged;
}

inline std::optional<ParsedArgs> parse_args(int argc, const char* argv[]) {
    ParsedArgs parsed;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            parsed.config_path = argv[++i];
        }
    }
    
    if (parsed.config_path) {
        auto cfg = load_config(*parsed.config_path);
        if (cfg) {
            parsed = apply_config(std::move(parsed), *cfg);
        }
    }
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            parsed.show_help = true;
            continue;
        }
        
        if (arg == "--config" && i + 1 < argc) {
            ++i;
            continue;
        }
        
        if (arg == "--cases" && i + 1 < argc) {
            parsed.cases = argv[++i];
            continue;
        }
        
        if (arg == "--model" && i + 1 < argc) {
            parsed.model = argv[++i];
            continue;
        }
        
        if (arg == "--permission-mode" && i + 1 < argc) {
            parsed.permission_mode = argv[++i];
            continue;
        }
        
        if (arg == "--max-cases" && i + 1 < argc) {
            parsed.max_cases = static_cast<size_t>(std::stoll(argv[++i]));
            continue;
        }
        
        if (arg == "--fail-on-regression") {
            parsed.fail_on_regression = true;
            continue;
        }
        
        if (arg == "--runs-per-case" && i + 1 < argc) {
            parsed.runs_per_case = static_cast<size_t>(std::stoll(argv[++i]));
            continue;
        }
        
        if (arg == "--baseline-label" && i + 1 < argc) {
            parsed.baseline_label = argv[++i];
            continue;
        }
        
        if (arg == "--baseline-cmd" && i + 1 < argc) {
            parsed.baseline_command = argv[++i];
            continue;
        }
        
        if (arg == "--baseline-args" && i + 1 < argc) {
            std::string args_str = argv[++i];
            std::vector<std::string> args_vec;
            std::istringstream iss(args_str);
            std::string token;
            while (iss >> token) {
                args_vec.push_back(token);
            }
            parsed.baseline_args = args_vec;
            continue;
        }
        
        if (arg == "--candidate-label" && i + 1 < argc) {
            parsed.candidate_label = argv[++i];
            continue;
        }
        
        if (arg == "--candidate-cmd" && i + 1 < argc) {
            parsed.candidate_command = argv[++i];
            continue;
        }
        
        if (arg == "--candidate-args" && i + 1 < argc) {
            std::string args_str = argv[++i];
            std::vector<std::string> args_vec;
            std::istringstream iss(args_str);
            std::string token;
            while (iss >> token) {
                args_vec.push_back(token);
            }
            parsed.candidate_args = args_vec;
            continue;
        }
        
        if (arg == "--workspace-mode" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "tmp-git") {
                parsed.workspace_mode = WorkspaceMode::TmpGit;
            } else if (mode == "worktree") {
                parsed.workspace_mode = WorkspaceMode::Worktree;
            } else if (mode == "current") {
                parsed.workspace_mode = WorkspaceMode::Current;
            }
            continue;
        }
    }
    
    return parsed;
}

inline void print_help() {
    std::cout << "Usage: pare-benchmark [options]\n\n"
              << "Options:\n"
              << "  --config <path>              Path to JSON config file\n"
              << "  --cases <path>               Path to test cases file\n"
              << "  --model <model>              Model to use\n"
              << "  --permission-mode <mode>     Permission mode (default: bypassPermissions)\n"
              << "  --max-cases <n>              Maximum number of cases to run\n"
              << "  --fail-on-regression         Fail on regression\n"
              << "  --runs-per-case <n>          Number of runs per case (default: 1)\n"
              << "  --baseline-label <name>      Baseline label\n"
              << "  --baseline-cmd <command>     Baseline command (default: bun)\n"
              << "  --baseline-args \"<args>\"     Baseline arguments (default: dist/cli.js)\n"
              << "  --candidate-label <name>     Candidate label\n"
              << "  --candidate-cmd <command>    Candidate command (default: bun)\n"
              << "  --candidate-args \"<args>\"    Candidate arguments (default: dist/cli.js)\n"
              << "  --workspace-mode <mode>      Workspace mode (tmp-git|worktree|current, default: tmp-git)\n"
              << "  --help, -h                   Show this help message\n";
}

inline std::string format_grouped(const GroupedItem& item) {
    double pass_rate = item.total > 0 ? (static_cast<double>(item.pass) / item.total) * 100 : 0;
    return "count=" + std::to_string(item.total) + " pass=" + std::to_string(pass_rate) + "% tokens=" + std::to_string(item.tokens);
}

inline int cli_main(int argc, const char* argv[]) {
    auto parsed_opt = parse_args(argc, argv);
    if (!parsed_opt) {
        std::cerr << "Error parsing arguments\n";
        return 1;
    }
    auto& parsed = *parsed_opt;
    
    if (parsed.show_help) {
        print_help();
        return 0;
    }
    
    auto result = run_benchmark(RunBenchmarkParams{
        .cases_path = parsed.cases,
        .model = parsed.model,
        .permission_mode = parsed.permission_mode,
        .max_cases = parsed.max_cases,
        .runs_per_case = parsed.runs_per_case,
        .baseline = VariantConfig{
            .label = parsed.baseline_label,
            .command = parsed.baseline_command,
            .args = parsed.baseline_args
        },
        .candidate = VariantConfig{
            .label = parsed.candidate_label,
            .command = parsed.candidate_command,
            .args = parsed.candidate_args
        },
        .fail_on_regression = parsed.fail_on_regression,
        .workspace_mode = parsed.workspace_mode
    });
    
    if (!result) {
        std::cerr << "Benchmark failed\n";
        return 1;
    }
    
    const auto& comp = result->comparison;
    const auto& base = comp.aggregate.baseline;
    const auto& cand = comp.aggregate.candidate;
    const auto& delta = comp.aggregate.delta;
    
    std::cout << "Pare benchmark finished\n\n";
    std::cout << "Artifacts: " << result->artifact_dir << "\n";
    std::cout << "Cases: " << base.total_count << "\n";
    std::cout << "Pass rate: baseline=" << (base.pass_rate * 100) << "% candidate=" << (cand.pass_rate * 100) << "% delta=" << (delta.pass_rate * 100) << "%\n";
    std::cout << "Execution errors: baseline=" << base.error_count << " (" << (base.error_rate * 100) << "%) candidate=" << cand.error_count << " (" << (cand.error_rate * 100) << "%) delta=" << (cand.error_count - base.error_count) << "\n";
    
    if (comp.aggregate.v2) {
        const auto& v2 = *comp.aggregate.v2;
        std::cout << "V2 metrics: runs_per_case=" << v2.runs_per_case << " evaluated=" << v2.evaluated_cases << " skipped=" << v2.skipped_cases << " unavailable=" << v2.unavailable_cases << " weighted_token_reduction=" << (v2.weighted_token_reduction_pct * 100) << "%\n";
    }
    
    if (cand.error_count > 0) {
        std::cout << "Top candidate errors:\n";
        for (const auto& [msg, cnt] : cand.top_errors) {
            std::cout << "- " << cnt << "x " << msg << "\n";
        }
    }
    
    std::cout << "Total tokens: baseline=" << base.tokens.total_tokens << " candidate=" << cand.tokens.total_tokens << " delta=" << delta.total_tokens << " (" << (delta.total_tokens_pct * 100) << "%)\n";
    std::cout << "Total duration: baseline=" << base.total_duration_ms << "ms candidate=" << cand.total_duration_ms << "ms delta=" << delta.total_duration_ms << "ms (" << (delta.total_duration_pct * 100) << "%)\n";
    std::cout << "Avg duration: baseline=" << base.avg_duration_ms << "ms candidate=" << cand.avg_duration_ms << "ms delta=" << delta.avg_duration_ms << "ms (" << (delta.avg_duration_pct * 100) << "%)\n";
    
    if (!comp.grouped.candidate.by_category.empty()) {
        std::cout << "\nGrouped by category (candidate):\n";
        for (const auto& [cat, item] : comp.grouped.candidate.by_category) {
            std::cout << "- " << cat << ": " << format_grouped(item) << "\n";
        }
    }
    
    if (!comp.grouped.candidate.by_frequency.empty()) {
        std::cout << "\nGrouped by frequency (candidate):\n";
        for (const auto& [freq, item] : comp.grouped.candidate.by_frequency) {
            std::cout << "- " << freq << ": " << format_grouped(item) << "\n";
        }
    }
    
    return 0;
}

} // namespace cc::benchmarks::pare
