// C++23 Module: Aggregates and merges tools/commands/clients from multiple sources
module;

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.merged_providers;


export namespace cc::hooks {


enum class ProviderSource {
    Builtin,
    Mcp,
    Plugin,
    Skill
};


[[nodiscard]] constexpr auto source_priority(ProviderSource src) -> int {
    switch (src) {
        case ProviderSource::Builtin: return 100;
        case ProviderSource::Mcp:     return 80;
        case ProviderSource::Plugin:  return 60;
        case ProviderSource::Skill:   return 40;
    }
    return 0;
}


struct MergedTool {
    std::string name;
    std::string description;
    std::string input_schema_json;
    ProviderSource source;
    std::string source_id;
    int priority{0};
    bool enabled{true};


    [[nodiscard]] auto qualified_name() const -> std::string {
        if (source == ProviderSource::Mcp && !source_id.empty()) {
            return std::format("{}:{}", source_id, name);
        }
        return name;
    }
};


struct MergedCommand {
    std::string name;
    std::string description;
    ProviderSource source;
    std::string source_id;
    int priority{0};
    std::vector<std::string> aliases;
    bool hidden{false};
};


struct MergedClient {
    std::string name;
    std::string endpoint;
    ProviderSource source;
    std::string source_id;
    bool connected{false};
    std::optional<std::string> version;
};


struct ProviderRegistration {
    std::string id;
    ProviderSource source;
    std::vector<MergedTool> tools;
    std::vector<MergedCommand> commands;
    std::vector<MergedClient> clients;
    std::chrono::system_clock::time_point registered_at;
};


using ProvidersChangeCallback = std::function<void()>;


class MergedProvidersHook {
public:
    MergedProvidersHook() = default;


    [[nodiscard]] auto get_all_tools() const -> std::vector<MergedTool> {
        return merge_and_dedupe_tools();
    }


    [[nodiscard]] auto get_all_commands() const -> std::vector<MergedCommand> {
        return merge_and_dedupe_commands();
    }


    [[nodiscard]] auto get_all_clients() const -> std::vector<MergedClient> {
        std::vector<MergedClient> result;
        for (const auto& reg : registrations_) {
            for (const auto& client : reg.clients) {
                result.push_back(client);
            }
        }
        return result;
    }


    auto add_provider(std::string_view source_id, ProviderSource source,
                      std::vector<MergedTool> tools,
                      std::vector<MergedCommand> commands,
                      std::vector<MergedClient> clients = {}) -> std::string {
        auto reg_id = std::format("{}_{}", format_source(source), source_id);


        if (source == ProviderSource::Mcp) {
            for (auto& tool : tools) {
                tool.source_id = std::string(source_id);
                tool.source = source;
                tool.priority = source_priority(source);
            }
        } else {
            for (auto& tool : tools) {
                tool.source_id = std::string(source_id);
                tool.source = source;
                tool.priority = source_priority(source);
            }
        }

        for (auto& cmd : commands) {
            cmd.source_id = std::string(source_id);
            cmd.source = source;
            cmd.priority = source_priority(source);
        }

        for (auto& client : clients) {
            client.source_id = std::string(source_id);
            client.source = source;
        }

        registrations_.push_back(ProviderRegistration{
            .id = reg_id,
            .source = source,
            .tools = std::move(tools),
            .commands = std::move(commands),
            .clients = std::move(clients),
            .registered_at = std::chrono::system_clock::now()
        });

        notify_change();
        return reg_id;
    }


    auto remove_provider(std::string_view source_id) -> bool {
        auto it = std::ranges::find_if(registrations_,
            [source_id](const auto& r) { return r.id == source_id; });
        if (it == registrations_.end()) return false;
        registrations_.erase(it);
        notify_change();
        return true;
    }


    auto refresh() -> void {
        last_refreshed_at_ = std::chrono::system_clock::now();
        ++refresh_generation_;
        notify_change();
    }


    [[nodiscard]] auto resolve_conflict(std::string_view name) const
        -> std::optional<MergedTool> {
        std::vector<MergedTool> candidates;
        for (const auto& reg : registrations_) {
            for (const auto& tool : reg.tools) {
                if (tool.name == name && tool.enabled) {
                    candidates.push_back(tool);
                }
            }
        }
        if (candidates.empty()) return std::nullopt;


        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.priority > b.priority;
        });
        return candidates.front();
    }


    [[nodiscard]] auto get_tools_by_source(ProviderSource source) const
        -> std::vector<MergedTool> {
        std::vector<MergedTool> result;
        for (const auto& reg : registrations_) {
            if (reg.source == source) {
                for (const auto& tool : reg.tools) {
                    result.push_back(tool);
                }
            }
        }
        return result;
    }


    [[nodiscard]] auto find_tool(std::string_view name) const
        -> std::optional<MergedTool> {

        for (const auto& reg : registrations_) {
            for (const auto& tool : reg.tools) {
                if (tool.name == name && tool.enabled) return tool;
            }
        }

        for (const auto& reg : registrations_) {
            for (const auto& tool : reg.tools) {
                if (tool.qualified_name() == name && tool.enabled) return tool;
            }
        }
        return std::nullopt;
    }


    [[nodiscard]] auto provider_count() const -> std::size_t {
        return registrations_.size();
    }

    [[nodiscard]] auto refresh_generation() const -> std::uint64_t {
        return refresh_generation_;
    }


    auto on_change(ProvidersChangeCallback cb) -> void {
        change_callbacks_.push_back(std::move(cb));
    }

private:
    std::vector<ProviderRegistration> registrations_;
    std::vector<ProvidersChangeCallback> change_callbacks_;
    std::chrono::system_clock::time_point last_refreshed_at_{};
    std::uint64_t refresh_generation_{0};


    [[nodiscard]] auto merge_and_dedupe_tools() const -> std::vector<MergedTool> {
        std::map<std::string, MergedTool> deduped;
        for (const auto& reg : registrations_) {
            for (const auto& tool : reg.tools) {
                if (!tool.enabled) continue;
                auto [it, inserted] = deduped.try_emplace(tool.name, tool);
                if (!inserted && tool.priority > it->second.priority) {
                    it->second = tool;
                }
            }
        }
        std::vector<MergedTool> result;
        result.reserve(deduped.size());
        for (auto& [_, tool] : deduped) {
            result.push_back(std::move(tool));
        }

        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });
        return result;
    }


    [[nodiscard]] auto merge_and_dedupe_commands() const -> std::vector<MergedCommand> {
        std::map<std::string, MergedCommand> deduped;
        for (const auto& reg : registrations_) {
            for (const auto& cmd : reg.commands) {
                auto [it, inserted] = deduped.try_emplace(cmd.name, cmd);
                if (!inserted && cmd.priority > it->second.priority) {
                    it->second = cmd;
                }
            }
        }
        std::vector<MergedCommand> result;
        result.reserve(deduped.size());
        for (auto& [_, cmd] : deduped) {
            result.push_back(std::move(cmd));
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });
        return result;
    }


    auto notify_change() -> void {
        for (const auto& cb : change_callbacks_) {
            if (cb) cb();
        }
    }


    [[nodiscard]] static auto format_source(ProviderSource src) -> std::string_view {
        switch (src) {
            case ProviderSource::Builtin: return "builtin";
            case ProviderSource::Mcp:     return "mcp";
            case ProviderSource::Plugin:  return "plugin";
            case ProviderSource::Skill:   return "skill";
        }
        return "unknown";
    }
};

} // namespace cc::hooks
