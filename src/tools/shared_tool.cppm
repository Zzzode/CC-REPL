module;

#include <string>
#include <string_view>
#include <unordered_map>

export module cc.tools.shared_tool;

export namespace cc::tools::shared {

struct SharedToolContext {
    std::string session_id;
    std::string cwd;
    std::unordered_map<std::string, std::string> metadata;
};

[[nodiscard]] inline auto describe_context(const SharedToolContext& context) -> std::string {
    std::string description = "session=" + (context.session_id.empty() ? std::string{"<none>"} : context.session_id);
    if (!context.cwd.empty()) {
        description += " cwd=" + context.cwd;
    }
    description += " metadata=" + std::to_string(context.metadata.size());
    return description;
}

[[nodiscard]] inline auto has_metadata(const SharedToolContext& context, std::string_view key) -> bool {
    return context.metadata.contains(std::string(key));
}

} // namespace cc::tools::shared
