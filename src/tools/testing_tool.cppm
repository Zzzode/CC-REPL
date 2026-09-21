module;

#include <string>
#include <string_view>
#include <vector>

export module cc.tools.testing_tool;

export namespace cc::tools::testing {

enum class TestStatus { Pending, Passed, Failed, Skipped };

struct TestCaseResult {
    std::string name;
    TestStatus status{TestStatus::Pending};
    std::string message;
};

struct TestRunSummary {
    int passed{0};
    int failed{0};
    int skipped{0};
};

[[nodiscard]] inline auto summarize(const std::vector<TestCaseResult>& results) -> TestRunSummary {
    TestRunSummary summary;
    for (const auto& result : results) {
        switch (result.status) {
            case TestStatus::Passed: ++summary.passed; break;
            case TestStatus::Failed: ++summary.failed; break;
            case TestStatus::Skipped: ++summary.skipped; break;
            case TestStatus::Pending: break;
        }
    }
    return summary;
}

[[nodiscard]] inline auto status_text(TestStatus status) -> std::string_view {
    switch (status) {
        case TestStatus::Pending: return "pending";
        case TestStatus::Passed: return "passed";
        case TestStatus::Failed: return "failed";
        case TestStatus::Skipped: return "skipped";
    }
    return "unknown";
}

} // namespace cc::tools::testing
