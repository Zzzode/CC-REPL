module;

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>
#include <variant>

export module cc.benchmarks.pare.case_loader;

import cc.benchmarks.pare.schema;
import cc.utils.json;
import cc.utils.crypto;

export namespace cc::benchmarks::pare {

namespace fs = std::filesystem;
namespace json = cc::utils::json;

struct LoadedCaseSet {
    PareCaseSet case_set;
    std::string absolute_path;
    std::string hash;
};

inline std::optional<NormalizeMode> parse_normalize_mode(const std::string& s) {
    if (s == "trim" || s == "lower" || s == "collapseWhitespace") {
        return s;
    }
    return std::nullopt;
}

inline std::optional<Assertion> parse_assertion(json::JsonVal val) {
    if (!val) return std::nullopt;
    
    auto type_val = val.get("type");
    if (!type_val || !type_val.is_str()) return std::nullopt;
    
    std::string type(type_val.as_str());
    
    if (type == "exact") {
        AssertionExact exact;
        auto expected_val = val.get("expected");
        if (expected_val && expected_val.is_str()) {
            exact.expected = std::string(expected_val.as_str());
        }
        
        auto normalize_val = val.get("normalize");
        if (normalize_val && normalize_val.is_arr()) {
            size_t arr_size = normalize_val.size();
            for (size_t i = 0; i < arr_size; ++i) {
                auto item = normalize_val.at(i);
                if (item && item.is_str()) {
                    auto mode = parse_normalize_mode(std::string(item.as_str()));
                    if (mode) {
                        exact.normalize.push_back(*mode);
                    }
                }
            }
        }
        
        return exact;
    }
    
    if (type == "regex") {
        AssertionRegex regex;
        auto pattern_val = val.get("pattern");
        if (pattern_val && pattern_val.is_str()) {
            regex.pattern = std::string(pattern_val.as_str());
        }
        
        auto flags_val = val.get("flags");
        if (flags_val && flags_val.is_str()) {
            regex.flags = std::string(flags_val.as_str());
        }
        
        return regex;
    }
    
    if (type == "includes") {
        AssertionIncludes includes;
        auto expected_val = val.get("expected");
        if (expected_val && expected_val.is_str()) {
            includes.expected = std::string(expected_val.as_str());
        }
        
        auto normalize_val = val.get("normalize");
        if (normalize_val && normalize_val.is_arr()) {
            size_t arr_size = normalize_val.size();
            for (size_t i = 0; i < arr_size; ++i) {
                auto item = normalize_val.at(i);
                if (item && item.is_str()) {
                    auto mode = parse_normalize_mode(std::string(item.as_str()));
                    if (mode) {
                        includes.normalize.push_back(*mode);
                    }
                }
            }
        }
        
        return includes;
    }
    
    return std::nullopt;
}

inline std::optional<CaseMetadata> parse_metadata(json::JsonVal val) {
    if (!val) return std::nullopt;
    
    CaseMetadata meta;
    
    auto pare_scenario_id_val = val.get("pareScenarioId");
    if (pare_scenario_id_val && pare_scenario_id_val.is_str()) {
        meta.pare_scenario_id = std::string(pare_scenario_id_val.as_str());
    }
    
    auto pare_scenario_name_val = val.get("pareScenarioName");
    if (pare_scenario_name_val && pare_scenario_name_val.is_str()) {
        meta.pare_scenario_name = std::string(pare_scenario_name_val.as_str());
    }
    
    auto category_val = val.get("category");
    if (category_val && category_val.is_str()) {
        meta.category = std::string(category_val.as_str());
    }
    
    auto use_frequency_val = val.get("useFrequency");
    if (use_frequency_val && use_frequency_val.is_str()) {
        meta.use_frequency = std::string(use_frequency_val.as_str());
    }
    
    auto command_val = val.get("command");
    if (command_val && command_val.is_str()) {
        meta.command = std::string(command_val.as_str());
    }
    
    return meta;
}

inline std::optional<PareCase> parse_case(json::JsonVal val) {
    if (!val) return std::nullopt;
    
    PareCase c;
    
    auto id_val = val.get("id");
    if (id_val && id_val.is_str()) {
        c.id = std::string(id_val.as_str());
    } else {
        return std::nullopt;
    }
    
    auto name_val = val.get("name");
    if (name_val && name_val.is_str()) {
        c.name = std::string(name_val.as_str());
    } else {
        return std::nullopt;
    }
    
    auto prompt_val = val.get("prompt");
    if (prompt_val && prompt_val.is_str()) {
        c.prompt = std::string(prompt_val.as_str());
    } else {
        return std::nullopt;
    }
    
    auto assertion_val = val.get("assertion");
    auto assertion = parse_assertion(assertion_val);
    if (!assertion) return std::nullopt;
    c.assertion = *assertion;
    
    auto tags_val = val.get("tags");
    if (tags_val && tags_val.is_arr()) {
        size_t arr_size = tags_val.size();
        for (size_t i = 0; i < arr_size; ++i) {
            auto item = tags_val.at(i);
            if (item && item.is_str()) {
                c.tags.push_back(std::string(item.as_str()));
            }
        }
    }
    
    auto metadata_val = val.get("metadata");
    auto meta = parse_metadata(metadata_val);
    if (meta) {
        c.metadata = *meta;
    }
    
    return c;
}

inline std::optional<PareCaseSet> parse_case_set(json::JsonVal val) {
    if (!val) return std::nullopt;
    
    PareCaseSet set;
    
    auto version_val = val.get("version");
    if (version_val && version_val.is_str()) {
        set.version = std::string(version_val.as_str());
    } else {
        return std::nullopt;
    }
    
    auto cases_val = val.get("cases");
    if (!cases_val || !cases_val.is_arr()) {
        return std::nullopt;
    }
    
    size_t cases_size = cases_val.size();
    for (size_t i = 0; i < cases_size; ++i) {
        auto case_val = cases_val.at(i);
        auto c = parse_case(case_val);
        if (c) {
            set.cases.push_back(*c);
        }
    }
    
    std::sort(set.cases.begin(), set.cases.end(),
             [](const PareCase& a, const PareCase& b) {
                 return a.id < b.id;
             });
    
    return set;
}

inline auto build_canonical_case_set_json(const PareCaseSet& case_set) -> std::string {
    json::JsonMutDoc doc;
    auto root = doc.object();
    auto cases = doc.array();
    for (const auto& c : case_set.cases) {
        auto case_obj = doc.object();
        case_obj.add("id", doc.string(c.id));
        case_obj.add("name", doc.string(c.name));
        case_obj.add("prompt", doc.string(c.prompt));

        auto tags = doc.array();
        for (const auto& tag : c.tags) tags.append(doc.string(tag));
        case_obj.add("tags", tags);

        if (c.metadata) {
            auto meta = doc.object();
            if (c.metadata->pare_scenario_id) meta.add("pareScenarioId", doc.string(*c.metadata->pare_scenario_id));
            if (c.metadata->pare_scenario_name) meta.add("pareScenarioName", doc.string(*c.metadata->pare_scenario_name));
            if (c.metadata->category) meta.add("category", doc.string(*c.metadata->category));
            if (c.metadata->use_frequency) meta.add("useFrequency", doc.string(*c.metadata->use_frequency));
            if (c.metadata->command) meta.add("command", doc.string(*c.metadata->command));
            case_obj.add("metadata", meta);
        }

        cases.append(case_obj);
    }
    root.add("version", doc.string(case_set.version));
    root.add("cases", cases);
    doc.set_root(root);
    return doc.to_string();
}

inline std::optional<LoadedCaseSet> load_case_set(const std::string& cases_path) {
    fs::path absolute_path;
    if (fs::path(cases_path).is_absolute()) {
        absolute_path = cases_path;
    } else {
        absolute_path = fs::current_path() / cases_path;
    }
    
    std::ifstream file(absolute_path);
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
    
    auto case_set_opt = parse_case_set(doc->root());
    
    if (!case_set_opt) {
        return std::nullopt;
    }
    
    std::string sorted_json = build_canonical_case_set_json(*case_set_opt);
    
    std::string hash = "sha256:" + cc::utils::crypto::sha256(sorted_json);
    
    return LoadedCaseSet{
        .case_set = *case_set_opt,
        .absolute_path = absolute_path.string(),
        .hash = hash
    };
}

} // namespace cc::benchmarks::pare
