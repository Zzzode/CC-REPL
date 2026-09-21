module;

#include <string>
#include <vector>
#include <algorithm>
#include <regex>
#include <optional>
#include <utility>
#include <variant>

export module cc.benchmarks.pare.evaluator;

import cc.benchmarks.pare.schema;

export namespace cc::benchmarks::pare {

inline std::string normalize(std::string value, const std::vector<NormalizeMode>& modes) {
    for (const auto& mode : modes) {
        if (mode == "trim") {
            auto start = value.find_first_not_of(" \t\n\r");
            auto end = value.find_last_not_of(" \t\n\r");
            if (start != std::string::npos && end != std::string::npos) {
                value = value.substr(start, end - start + 1);
            } else {
                value.clear();
            }
        } else if (mode == "lower") {
            std::transform(value.begin(), value.end(), value.begin(),
                          [](unsigned char c) { return std::tolower(c); });
        } else if (mode == "collapseWhitespace") {
            std::string result;
            bool in_space = false;
            for (char c : value) {
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (!in_space) {
                        result += ' ';
                        in_space = true;
                    }
                } else {
                    result += c;
                    in_space = false;
                }
            }
            value = std::move(result);
        }
    }
    return value;
}

inline std::pair<bool, std::optional<std::string>> evaluate_assertion(
    const std::string& text,
    const Assertion& assertion) {
    
    if (std::holds_alternative<AssertionExact>(assertion)) {
        const auto& exact = std::get<AssertionExact>(assertion);
        auto actual = normalize(text, exact.normalize);
        auto expected = normalize(exact.expected, exact.normalize);
        if (actual == expected) {
            return {true, std::nullopt};
        } else {
            return {false, "expected exact match"};
        }
    }
    
    if (std::holds_alternative<AssertionIncludes>(assertion)) {
        const auto& includes = std::get<AssertionIncludes>(assertion);
        auto actual = normalize(text, includes.normalize);
        auto expected = normalize(includes.expected, includes.normalize);
        if (actual.find(expected) != std::string::npos) {
            return {true, std::nullopt};
        } else {
            return {false, "expected text to include substring"};
        }
    }
    
    if (std::holds_alternative<AssertionRegex>(assertion)) {
        const auto& regex_assertion = std::get<AssertionRegex>(assertion);
        std::regex::flag_type flags = std::regex::ECMAScript;
        if (regex_assertion.flags) {
            for (char c : *regex_assertion.flags) {
                if (c == 'i') flags |= std::regex::icase;
                if (c == 'm') flags |= std::regex::multiline;
            }
        }
        try {
            std::regex pattern(regex_assertion.pattern, flags);
            if (std::regex_search(text, pattern)) {
                return {true, std::nullopt};
            } else {
                return {false, "expected text to match regex"};
            }
        } catch (const std::regex_error& e) {
            return {false, std::string("regex error: ") + e.what()};
        }
    }
    
    return {false, "unknown assertion type"};
}

} // namespace cc::benchmarks::pare
