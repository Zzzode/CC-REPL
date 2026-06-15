module;

#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <functional>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

export module cc.benchmarks.pare.execute_ref;

import cc.benchmarks.pare.schema;
import cc.benchmarks.pare.evaluator;
import cc.benchmarks.pare.metrics;
import cc.utils.json;
import cc.utils.bash_execution;

export namespace cc::benchmarks::pare {

namespace json = cc::utils::json;

struct LocalExecResult {
    int exit_code = 0;
    std::string stdout_output;
    std::string stderr_output;
};

inline std::string shell_quote(std::string_view value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

inline LocalExecResult execute_sync(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::optional<std::string>& cwd
) {
    std::string shell_command;
    if (cwd && !cwd->empty()) {
        shell_command += "cd " + shell_quote(*cwd) + " && ";
    }
    shell_command += shell_quote(command);
    for (const auto& arg : args) {
        shell_command += " " + shell_quote(arg);
    }
    shell_command += " 2>&1";

    LocalExecResult result;
    std::array<char, 4096> buffer{};
    FILE* pipe = cc::utils::bash::popen_spawn(shell_command.c_str());
    if (!pipe) {
        result.exit_code = 127;
        result.stderr_output = "failed to launch command";
        return result;
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.stdout_output += buffer.data();
    }
    int status = cc::utils::bash::pclose_spawn(pipe);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = status;
    }
    return result;
}

inline std::string get_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

inline std::string try_parse_json_output(const std::string& raw) {
    return raw;
}

inline std::string extract_assistant_text(json::JsonVal payload, const std::string& fallback_raw) {
    if (!payload) return fallback_raw;
    
    auto result_val = payload.get("result");
    if (result_val && result_val.is_str()) {
        return std::string(result_val.as_str());
    }
    
    auto message_val = payload.get("message");
    if (message_val && message_val.is_str()) {
        return std::string(message_val.as_str());
    }
    
    auto text_val = payload.get("text");
    if (text_val && text_val.is_str()) {
        return std::string(text_val.as_str());
    }
    
    auto content_val = payload.get("message").get("content");
    if (content_val && content_val.is_arr()) {
        size_t arr_size = content_val.size();
        for (size_t i = 0; i < arr_size; ++i) {
            auto item = content_val.at(i);
            auto text_item = item.get("text");
            if (text_item && text_item.is_str()) {
                return std::string(text_item.as_str());
            }
        }
    }
    
    return fallback_raw;
}

inline CaseRunResult execute_single_run(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::optional<std::string>& model,
    const std::string& permission_mode,
    const PareCase& c,
    const std::optional<std::string>& cwd) {
    
    CaseRunResult result;
    result.case_id = c.id;
    result.case_name = c.name;
    result.metadata = c.metadata;
    
    std::vector<std::string> runner_args = args;
    runner_args.push_back("--print");
    runner_args.push_back("--output-format");
    runner_args.push_back("json");
    runner_args.push_back("--permission-mode");
    runner_args.push_back(permission_mode);
    if (model) {
        runner_args.push_back("--model");
        runner_args.push_back(*model);
    }
    runner_args.push_back(c.prompt);
    
    auto start = std::chrono::steady_clock::now();
    
    auto exec_result = execute_sync(command, runner_args, cwd);
    
    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    result.duration_ms = duration_ms;
    
    std::string raw_output = exec_result.stdout_output + exec_result.stderr_output;
    result.raw_output = raw_output;
    
    auto doc = json::parse(exec_result.stdout_output);
    auto payload = doc ? doc->root() : json::JsonVal{};
    
    std::string assistant_text = extract_assistant_text(payload, raw_output);
    result.assistant_text = assistant_text;
    
    auto assertion_result = evaluate_assertion(assistant_text, c.assertion);
    
    bool is_error = false;
    if (payload) {
        auto is_error_val = payload.get("is_error");
        if (is_error_val && is_error_val.is_bool()) {
            is_error = is_error_val.as_bool();
        }
    }
    if (!is_error && exec_result.exit_code != 0) {
        is_error = true;
    }
    
    result.pass = !is_error && assertion_result.first;
    if (!result.pass) {
        if (is_error) {
            result.error = "process exit code " + std::to_string(exec_result.exit_code);
        } else {
            result.error = assertion_result.second;
        }
    }
    
    result.usage = extract_usage(payload);
    
    return result;
}

struct ExecuteVariantParams {
    std::string label;
    std::string command;
    std::vector<std::string> args;
    std::optional<std::string> model;
    std::string permission_mode;
    std::optional<size_t> max_cases;
    size_t runs_per_case = 1;
    std::string cases_path;
    std::string case_set_hash;
    std::vector<PareCase> cases;
    std::function<void(const CaseRunResult&)> on_case_result;
    std::optional<std::string> cwd;
};

inline VariantRun execute_variant(const ExecuteVariantParams& params) {
    VariantRun run;
    run.label = params.label;
    run.command = params.command;
    run.args = params.args;
    run.model = params.model;
    run.permission_mode = params.permission_mode;
    run.cases_path = params.cases_path;
    run.case_set_hash = params.case_set_hash;
    run.started_at = get_iso8601();
    
    std::vector<PareCase> selected_cases;
    if (params.max_cases && *params.max_cases < params.cases.size()) {
        selected_cases = std::vector<PareCase>(params.cases.begin(), params.cases.begin() + *params.max_cases);
    } else {
        selected_cases = params.cases;
    }
    
    size_t runs_per_case = std::max<size_t>(1, params.runs_per_case);
    
    for (size_t case_idx = 0; case_idx < selected_cases.size(); ++case_idx) {
        const auto& c = selected_cases[case_idx];
        std::cout << "[pare-benchmark] " << params.label << " " << (case_idx + 1) << "/" << selected_cases.size() << " " << c.id << std::endl;
        
        std::vector<CaseRunResult> run_results;
        for (size_t run_idx = 0; run_idx < runs_per_case; ++run_idx) {
            auto single_result = execute_single_run(
                params.command, params.args, params.model, params.permission_mode, c, params.cwd);
            run_results.push_back(single_result);
        }
        
        size_t pass_count = 0;
        for (const auto& r : run_results) {
            if (r.pass) ++pass_count;
        }
        
        CaseRunResult final_result = run_results[(run_results.size() - 1) / 2];
        final_result.pass = pass_count >= (run_results.size() + 1) / 2;
        final_result.usage = median_usage(run_results);
        
        std::vector<double> durations;
        for (const auto& r : run_results) {
            if (r.duration_ms) {
                durations.push_back(static_cast<double>(*r.duration_ms));
            }
        }
        auto median_dur = median(durations);
        if (median_dur) {
            final_result.duration_ms = static_cast<int64_t>(*median_dur);
        }
        
        bool all_unavailable_or_zero = true;
        for (const auto& r : run_results) {
            if (r.status != CaseRunStatus::Unavailable && r.usage.total_tokens != 0) {
                all_unavailable_or_zero = false;
                break;
            }
        }
        if (all_unavailable_or_zero) {
            final_result.status = CaseRunStatus::Unavailable;
        } else {
            final_result.status = CaseRunStatus::Evaluated;
        }
        
        for (const auto& r : run_results) {
            SingleRun sr;
            sr.pass = r.pass;
            sr.error = r.error;
            sr.usage = r.usage;
            sr.duration_ms = r.duration_ms;
            final_result.runs.push_back(sr);
        }
        
        run.results.push_back(final_result);
        
        if (params.on_case_result) {
            params.on_case_result(final_result);
        }
    }
    
    run.ended_at = get_iso8601();
    return run;
}

} // namespace cc::benchmarks::pare
