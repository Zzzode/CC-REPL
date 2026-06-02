module;

#include <vector>
#include <algorithm>
#include <cmath>
#include <string>
#include <optional>

export module cc.benchmarks.pare.metrics;

import cc.benchmarks.pare.schema;
import cc.utils.json;

export namespace cc::benchmarks::pare {

namespace json = cc::utils::json;

inline int64_t to_number(const std::string& s) {
    try {
        return std::stoll(s);
    } catch (...) {
        return 0;
    }
}

inline int64_t to_number(json::JsonVal val) {
    if (!val) return 0;
    if (val.is_num()) {
        return val.as_int();
    } else if (val.is_str()) {
        return to_number(std::string(val.as_str()));
    }
    return 0;
}

inline NormalizedUsage extract_usage(json::JsonVal payload) {
    NormalizedUsage usage;
    
    if (!payload) return usage;
    
    auto usage_val = payload.get("usage");
    if (!usage_val) usage_val = payload;
    
    usage.input_tokens = to_number(usage_val.get("input_tokens"));
    if (usage.input_tokens == 0) {
        usage.input_tokens = to_number(usage_val.get("inputTokens"));
    }
    
    usage.output_tokens = to_number(usage_val.get("output_tokens"));
    if (usage.output_tokens == 0) {
        usage.output_tokens = to_number(usage_val.get("outputTokens"));
    }
    
    usage.cache_read_input_tokens = to_number(usage_val.get("cache_read_input_tokens"));
    if (usage.cache_read_input_tokens == 0) {
        usage.cache_read_input_tokens = to_number(usage_val.get("cacheReadInputTokens"));
    }
    
    usage.cache_creation_input_tokens = to_number(usage_val.get("cache_creation_input_tokens"));
    if (usage.cache_creation_input_tokens == 0) {
        usage.cache_creation_input_tokens = to_number(usage_val.get("cacheCreationInputTokens"));
    }
    
    usage.total_tokens = to_number(usage_val.get("total_tokens"));
    if (usage.total_tokens == 0) {
        usage.total_tokens = to_number(usage_val.get("totalTokens"));
    }
    
    if (usage.total_tokens == 0) {
        usage.total_tokens = usage.input_tokens + usage.output_tokens + 
                            usage.cache_read_input_tokens + usage.cache_creation_input_tokens;
    }
    
    return usage;
}

inline NormalizedUsage sum_usage(const std::vector<NormalizedUsage>& usages) {
    NormalizedUsage result;
    for (const auto& u : usages) {
        result.input_tokens += u.input_tokens;
        result.output_tokens += u.output_tokens;
        result.cache_read_input_tokens += u.cache_read_input_tokens;
        result.cache_creation_input_tokens += u.cache_creation_input_tokens;
        result.total_tokens += u.total_tokens;
    }
    return result;
}

inline std::optional<double> p50(const std::vector<double>& values) {
    if (values.empty()) return std::nullopt;
    
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    
    size_t mid = sorted.size() / 2;
    if (sorted.size() % 2 == 1) {
        return sorted[mid];
    } else {
        return (sorted[mid - 1] + sorted[mid]) / 2.0;
    }
}

inline std::optional<double> median(const std::vector<double>& values) {
    return p50(values);
}

inline NormalizedUsage median_usage(const std::vector<CaseRunResult>& runs) {
    std::vector<double> input_tokens, output_tokens, cache_read, cache_creation, total_tokens;
    
    for (const auto& r : runs) {
        input_tokens.push_back(static_cast<double>(r.usage.input_tokens));
        output_tokens.push_back(static_cast<double>(r.usage.output_tokens));
        cache_read.push_back(static_cast<double>(r.usage.cache_read_input_tokens));
        cache_creation.push_back(static_cast<double>(r.usage.cache_creation_input_tokens));
        total_tokens.push_back(static_cast<double>(r.usage.total_tokens));
    }
    
    NormalizedUsage result;
    result.input_tokens = static_cast<int64_t>(median(input_tokens).value_or(0.0));
    result.output_tokens = static_cast<int64_t>(median(output_tokens).value_or(0.0));
    result.cache_read_input_tokens = static_cast<int64_t>(median(cache_read).value_or(0.0));
    result.cache_creation_input_tokens = static_cast<int64_t>(median(cache_creation).value_or(0.0));
    result.total_tokens = static_cast<int64_t>(median(total_tokens).value_or(0.0));
    
    return result;
}

} // namespace cc::benchmarks::pare
