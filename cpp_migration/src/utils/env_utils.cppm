module;
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.env_utils;

export namespace cc::utils {

[[nodiscard]] inline std::string trim_env_value(std::string_view value) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    auto begin = std::find_if(value.begin(), value.end(), not_space);
    auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

[[nodiscard]] inline std::string lower_env_value(std::string_view value) {
    std::string normalized = trim_env_value(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return normalized;
}

[[nodiscard]] inline bool is_env_truthy(std::string_view env_var) {
    const auto normalized = lower_env_value(env_var);
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

[[nodiscard]] inline bool is_env_truthy(bool env_var) noexcept {
    return env_var;
}

[[nodiscard]] inline bool is_env_truthy(const char* env_var) {
    if (env_var == nullptr) return false;
    return is_env_truthy(std::string_view(env_var));
}

[[nodiscard]] inline bool is_env_truthy(std::nullopt_t) noexcept {
    return false;
}

[[nodiscard]] inline bool is_env_defined_falsy(std::string_view env_var) {
    if (env_var.empty()) return false;
    const auto normalized = lower_env_value(env_var);
    return normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off";
}

[[nodiscard]] inline bool is_env_defined_falsy(bool env_var) noexcept {
    return !env_var;
}

[[nodiscard]] inline bool is_env_defined_falsy(const char* env_var) {
    if (env_var == nullptr) return false;
    return is_env_defined_falsy(std::string_view(env_var));
}

[[nodiscard]] inline bool is_env_defined_falsy(std::nullopt_t) noexcept {
    return false;
}

[[nodiscard]] inline bool has_node_option(std::string_view node_options, std::string_view flag) {
    std::size_t i = 0;
    while (i < node_options.size()) {
        while (i < node_options.size() && std::isspace(static_cast<unsigned char>(node_options[i]))) ++i;
        const auto start = i;
        while (i < node_options.size() && !std::isspace(static_cast<unsigned char>(node_options[i]))) ++i;
        if (start < i && node_options.substr(start, i - start) == flag) return true;
    }
    return false;
}

[[nodiscard]] inline bool is_bare_mode(std::string_view claude_code_simple, const std::vector<std::string>& argv) {
    if (is_env_truthy(claude_code_simple)) return true;
    return std::find(argv.begin(), argv.end(), "--bare") != argv.end();
}

[[nodiscard]] inline std::expected<std::map<std::string, std::string>, std::string> parse_env_vars(const std::vector<std::string>& raw_env_args) {
    std::map<std::string, std::string> parsed;
    for (const auto& env_str : raw_env_args) {
        const auto eq = env_str.find('=');
        if (eq == std::string::npos || eq == 0) {
            return std::unexpected("Invalid environment variable format: " + env_str + ", environment variables should be added as: -e KEY1=value1 -e KEY2=value2");
        }
        parsed[env_str.substr(0, eq)] = env_str.substr(eq + 1);
    }
    return parsed;
}

[[nodiscard]] inline std::string resolve_aws_region_impl(std::optional<std::string_view> aws_region, std::optional<std::string_view> aws_default_region) {
    if (aws_region.has_value() && !aws_region->empty()) return std::string(*aws_region);
    if (aws_default_region.has_value() && !aws_default_region->empty()) return std::string(*aws_default_region);
    return "us-east-1";
}

[[nodiscard]] inline std::string resolve_aws_region(std::string_view aws_region, std::string_view aws_default_region) {
    return resolve_aws_region_impl(std::optional<std::string_view>{aws_region}, std::optional<std::string_view>{aws_default_region});
}

[[nodiscard]] inline std::string resolve_aws_region(std::nullopt_t, std::string_view aws_default_region) {
    return resolve_aws_region_impl(std::nullopt, std::optional<std::string_view>{aws_default_region});
}

[[nodiscard]] inline std::string resolve_aws_region(std::nullopt_t, std::nullopt_t) {
    return resolve_aws_region_impl(std::nullopt, std::nullopt);
}

[[nodiscard]] inline std::string resolve_default_vertex_region_impl(std::optional<std::string_view> cloud_ml_region) {
    if (cloud_ml_region.has_value() && !cloud_ml_region->empty()) return std::string(*cloud_ml_region);
    return "us-east5";
}

[[nodiscard]] inline std::string resolve_default_vertex_region(std::string_view cloud_ml_region) {
    return resolve_default_vertex_region_impl(std::optional<std::string_view>{cloud_ml_region});
}

[[nodiscard]] inline std::string resolve_default_vertex_region(std::nullopt_t) {
    return resolve_default_vertex_region_impl(std::optional<std::string_view>{});
}

// Get environment variable with a default fallback
std::string get_env_or(std::string_view key, std::string_view default_val) {
    const char* val = std::getenv(std::string(key).c_str());
    if (val) return std::string(val);
    return std::string(default_val);
}

// Get environment variable as boolean
bool get_env_bool(std::string_view key, bool default_val) {
    const char* val = std::getenv(std::string(key).c_str());
    if (!val) return default_val;

    std::string_view sv(val);
    return sv == "1" || sv == "true" || sv == "yes" || sv == "on";
}

// Get environment variable as integer
int get_env_int(std::string_view key, int default_val) {
    const char* val = std::getenv(std::string(key).c_str());
    if (!val) return default_val;

    try {
        return std::stoi(std::string(val));
    } catch (...) {
        return default_val;
    }
}

// Require an environment variable, returning error if not set
std::expected<std::string, std::string> require_env(std::string_view key) {
    const char* val = std::getenv(std::string(key).c_str());
    if (!val || std::string_view(val).empty()) {
        return std::unexpected("Required environment variable not set: " + std::string(key));
    }
    return std::string(val);
}

} // namespace cc::utils
