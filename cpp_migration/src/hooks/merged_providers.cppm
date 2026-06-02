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

// 提供者来源类型
enum class ProviderSource {
    Builtin,    // 内置工具/命令
    Mcp,        // MCP 服务器提供
    Plugin,     // 插件提供
    Skill       // Skill 提供
};

// 优先级：来源的默认优先级 (越高越优先)
[[nodiscard]] constexpr auto source_priority(ProviderSource src) -> int {
    switch (src) {
        case ProviderSource::Builtin: return 100;
        case ProviderSource::Mcp:     return 80;
        case ProviderSource::Plugin:  return 60;
        case ProviderSource::Skill:   return 40;
    }
    return 0;
}

// 合并后的工具描述
struct MergedTool {
    std::string name;                    // 工具名（可能带命名空间前缀）
    std::string description;
    std::string input_schema_json;       // JSON Schema 字符串
    ProviderSource source;
    std::string source_id;               // 来源标识（如 MCP server name）
    int priority{0};                     // 冲突解决优先级
    bool enabled{true};

    // 完全限定名（带命名空间前缀）
    [[nodiscard]] auto qualified_name() const -> std::string {
        if (source == ProviderSource::Mcp && !source_id.empty()) {
            return std::format("{}:{}", source_id, name);
        }
        return name;
    }
};

// 合并后的命令描述
struct MergedCommand {
    std::string name;                    // 命令名（如 "/help"）
    std::string description;
    ProviderSource source;
    std::string source_id;
    int priority{0};
    std::vector<std::string> aliases;    // 命令别名
    bool hidden{false};                  // 是否在帮助中隐藏
};

// 合并后的 API 客户端描述
struct MergedClient {
    std::string name;                    // 客户端标识
    std::string endpoint;                // API 端点
    ProviderSource source;
    std::string source_id;
    bool connected{false};               // 连接状态
    std::optional<std::string> version;  // 协议版本
};

// 单个提供者注册的资源集合
struct ProviderRegistration {
    std::string id;                      // 唯一注册 ID
    ProviderSource source;
    std::vector<MergedTool> tools;
    std::vector<MergedCommand> commands;
    std::vector<MergedClient> clients;
    std::chrono::system_clock::time_point registered_at;
};

// 变更事件回调
using ProvidersChangeCallback = std::function<void()>;

// ─── MergedProvidersHook: 多源聚合管理类 ───────────────────────
class MergedProvidersHook {
public:
    MergedProvidersHook() = default;

    // 获取所有合并后的工具（已去重，按优先级排序）
    [[nodiscard]] auto get_all_tools() const -> std::vector<MergedTool> {
        return merge_and_dedupe_tools();
    }

    // 获取所有合并后的命令（已去重）
    [[nodiscard]] auto get_all_commands() const -> std::vector<MergedCommand> {
        return merge_and_dedupe_commands();
    }

    // 获取所有合并后的客户端
    [[nodiscard]] auto get_all_clients() const -> std::vector<MergedClient> {
        std::vector<MergedClient> result;
        for (const auto& reg : registrations_) {
            for (const auto& client : reg.clients) {
                result.push_back(client);
            }
        }
        return result;
    }

    // 添加一个提供者（注册其工具、命令、客户端）
    auto add_provider(std::string_view source_id, ProviderSource source,
                      std::vector<MergedTool> tools,
                      std::vector<MergedCommand> commands,
                      std::vector<MergedClient> clients = {}) -> std::string {
        auto reg_id = std::format("{}_{}", format_source(source), source_id);

        // 为 MCP 工具添加命名空间前缀
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

    // 移除一个提供者
    auto remove_provider(std::string_view source_id) -> bool {
        auto it = std::ranges::find_if(registrations_,
            [source_id](const auto& r) { return r.id == source_id; });
        if (it == registrations_.end()) return false;
        registrations_.erase(it);
        notify_change();
        return true;
    }

    // 刷新所有提供者（触发重新加载）
    auto refresh() -> void {
        last_refreshed_at_ = std::chrono::system_clock::now();
        ++refresh_generation_;
        notify_change();
    }

    // 解决工具名冲突：返回优先级最高的工具
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

        // 按优先级降序排序，取第一个
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.priority > b.priority;
        });
        return candidates.front();
    }

    // 按来源筛选工具
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

    // 查找工具（支持前缀匹配和精确匹配）
    [[nodiscard]] auto find_tool(std::string_view name) const
        -> std::optional<MergedTool> {
        // 先精确匹配
        for (const auto& reg : registrations_) {
            for (const auto& tool : reg.tools) {
                if (tool.name == name && tool.enabled) return tool;
            }
        }
        // 再匹配 qualified_name
        for (const auto& reg : registrations_) {
            for (const auto& tool : reg.tools) {
                if (tool.qualified_name() == name && tool.enabled) return tool;
            }
        }
        return std::nullopt;
    }

    // 获取注册数量
    [[nodiscard]] auto provider_count() const -> std::size_t {
        return registrations_.size();
    }

    [[nodiscard]] auto refresh_generation() const -> std::uint64_t {
        return refresh_generation_;
    }

    // 注册变更回调
    auto on_change(ProvidersChangeCallback cb) -> void {
        change_callbacks_.push_back(std::move(cb));
    }

private:
    std::vector<ProviderRegistration> registrations_;
    std::vector<ProvidersChangeCallback> change_callbacks_;
    std::chrono::system_clock::time_point last_refreshed_at_{};
    std::uint64_t refresh_generation_{0};

    // 合并并去重工具列表（同名工具取优先级最高者）
    [[nodiscard]] auto merge_and_dedupe_tools() const -> std::vector<MergedTool> {
        std::map<std::string, MergedTool> deduped;
        for (const auto& reg : registrations_) {
            for (const auto& tool : reg.tools) {
                if (!tool.enabled) continue;
                auto [it, inserted] = deduped.try_emplace(tool.name, tool);
                if (!inserted && tool.priority > it->second.priority) {
                    it->second = tool; // 高优先级覆盖
                }
            }
        }
        std::vector<MergedTool> result;
        result.reserve(deduped.size());
        for (auto& [_, tool] : deduped) {
            result.push_back(std::move(tool));
        }
        // 按名称排序输出
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });
        return result;
    }

    // 合并并去重命令列表
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

    // 通知变更
    auto notify_change() -> void {
        for (const auto& cb : change_callbacks_) {
            if (cb) cb();
        }
    }

    // 格式化来源名
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
