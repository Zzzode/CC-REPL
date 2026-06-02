// C++23 Module: Runtime settings state management with layered config and persistence
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module cc.hooks.settings_hooks;

import cc.utils.json;

export namespace cc::hooks {

// 配置层级（优先级从低到高）
enum class SettingLayer {
    Defaults,   // 硬编码默认值
    Global,     // 全局用户配置 (~/.config/cc/settings.json)
    Project,    // 项目级配置 (.cc/settings.json)
    Env,        // 环境变量覆盖
    Runtime     // 运行时动态设置（最高优先级）
};

// 设置值类型（类型安全联合体）
using SettingValue = std::variant<
    bool,
    int64_t,
    double,
    std::string,
    std::vector<std::string>
>;

// 设置值类型枚举（用于 schema 描述）
enum class SettingType { Bool, Int, Float, String, StringArray };

// 设置项的 schema 定义
struct SettingSchema {
    std::string key;                            // 设置键 (e.g., "model.name")
    SettingType type{SettingType::String};
    SettingValue default_value;                  // 默认值
    std::string description;                    // 描述文本
    std::optional<std::string> group;           // 分组名（用于 UI 分类）
    bool hidden{false};                         // 是否在 UI 中隐藏
    bool restart_required{false};               // 修改后是否需要重启

    // 校验函数（可选）: 返回空表示通过，非空为错误信息
    std::function<std::optional<std::string>(const SettingValue&)> validator;
};

// 单层级中的设置条目
struct LayeredEntry {
    SettingLayer layer;
    SettingValue value;
};

// 设置 hook 内部状态
struct SettingsHookState {
    // 每个 key 在各层级的值（按优先级排列）
    std::map<std::string, std::vector<LayeredEntry>> entries;
    // Schema 注册表
    std::map<std::string, SettingSchema> schemas;
};

// 订阅回调类型
using SettingChangeCallback = std::function<void(std::string_view key, const SettingValue& new_value)>;
// 取消订阅函数
using UnsubscribeFn = std::function<void()>;

// ─── SettingsHook: 核心设置管理类 ──────────────────────────────
class SettingsHook {
public:
    SettingsHook() = default;

    // 注册设置项 schema
    auto register_schema(SettingSchema schema) -> void {
        auto key = schema.key;
        // 将默认值写入 Defaults 层
        set_at_layer(key, SettingLayer::Defaults, schema.default_value);
        state_.schemas.emplace(std::move(key), std::move(schema));
    }

    // 批量注册 schema
    auto register_schemas(std::vector<SettingSchema> schemas) -> void {
        for (auto& s : schemas) {
            register_schema(std::move(s));
        }
    }

    // 类型安全获取设置值（返回有效层级的最高优先级值）
    template<typename T>
    [[nodiscard]] auto get(std::string_view key) const -> T {
        auto effective = get_effective_value(key);
        if (effective.has_value()) {
            if (auto* val = std::get_if<T>(&*effective)) {
                return *val;
            }
        }
        // 回退到 schema 默认值
        auto it = state_.schemas.find(std::string(key));
        if (it != state_.schemas.end()) {
            if (auto* val = std::get_if<T>(&it->second.default_value)) {
                return *val;
            }
        }
        return T{}; // 类型默认值
    }

    // 在 Runtime 层设置值
    auto set(std::string_view key, SettingValue value) -> std::expected<void, std::string> {
        // 先校验
        auto validation = validate(key, value);
        if (!validation.has_value()) {
            return std::unexpected(validation.error());
        }
        set_at_layer(std::string(key), SettingLayer::Runtime, std::move(value));
        notify_subscribers(key);
        return {};
    }

    // 在指定层级设置值
    auto set_at(std::string_view key, SettingLayer layer, SettingValue value)
        -> std::expected<void, std::string> {
        auto validation = validate(key, value);
        if (!validation.has_value()) {
            return std::unexpected(validation.error());
        }
        set_at_layer(std::string(key), layer, std::move(value));
        notify_subscribers(key);
        return {};
    }

    // 重置指定 key 的 Runtime 层值
    auto reset(std::string_view key) -> void {
        auto it = state_.entries.find(std::string(key));
        if (it == state_.entries.end()) return;
        // 移除 Runtime 层
        std::erase_if(it->second,
            [](const auto& e) { return e.layer == SettingLayer::Runtime; });
        notify_subscribers(key);
    }

    // 重置所有 Runtime 层设置
    auto reset_all() -> void {
        for (auto& [key, entries] : state_.entries) {
            std::erase_if(entries,
                [](const auto& e) { return e.layer == SettingLayer::Runtime; });
        }
        // 通知所有订阅者
        for (const auto& [key, _] : subscribers_) {
            notify_subscribers(key);
        }
    }

    // 获取某个 key 的生效层级
    [[nodiscard]] auto get_effective_layer(std::string_view key) const
        -> std::optional<SettingLayer> {
        auto it = state_.entries.find(std::string(key));
        if (it == state_.entries.end() || it->second.empty()) return std::nullopt;
        // 取优先级最高的层
        auto max_it = std::max_element(it->second.begin(), it->second.end(),
            [](const auto& a, const auto& b) {
                return static_cast<int>(a.layer) < static_cast<int>(b.layer);
            });
        return max_it->layer;
    }

    // 订阅特定 key 的变更
    [[nodiscard]] auto subscribe(std::string_view key, SettingChangeCallback cb)
        -> UnsubscribeFn {
        auto id = next_sub_id_++;
        auto k = std::string(key);
        subscribers_[k].emplace_back(id, std::move(cb));
        return [this, k, id]() {
            auto it = subscribers_.find(k);
            if (it != subscribers_.end()) {
                std::erase_if(it->second,
                    [id](const auto& p) { return p.first == id; });
            }
        };
    }

    // 校验设置值
    [[nodiscard]] auto validate(std::string_view key, const SettingValue& value) const
        -> std::expected<void, std::string> {
        auto it = state_.schemas.find(std::string(key));
        if (it == state_.schemas.end()) {
            // 无 schema 注册，允许任意值
            return {};
        }
        // 类型检查
        if (!type_matches(it->second.type, value)) {
            return std::unexpected(
                std::format("Type mismatch for '{}': expected {}", key,
                           type_name(it->second.type)));
        }
        // 自定义验证器
        if (it->second.validator) {
            auto err = it->second.validator(value);
            if (err.has_value()) {
                return std::unexpected(*err);
            }
        }
        return {};
    }

    // 导出所有运行时 + 项目 + 全局层设置为 JSON 字符串
    [[nodiscard]] auto export_settings() const -> std::string {
        namespace json = cc::utils::json;

        json::JsonMutDoc doc;
        auto root = doc.object();
        for (const auto& [key, entries] : state_.entries) {
            (void)entries;
            auto effective = get_effective_value(key);
            if (!effective.has_value()) continue;
            root.add(key, make_json_value(doc, *effective));
        }
        doc.set_root(root);
        return doc.to_pretty_string();
    }

    // 从 JSON 字符串导入设置到 Runtime 层
    auto import_settings(std::string_view json) -> std::expected<void, std::string> {
        if (json.empty()) {
            return std::unexpected("Empty JSON input");
        }

        auto parsed = cc::utils::json::parse(json);
        if (!parsed) {
            return std::unexpected(parsed.error().format());
        }

        auto root = parsed->root();
        if (!root.is_obj()) {
            return std::unexpected("Settings JSON must be an object");
        }

        std::optional<std::string> first_error;
        root.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
            if (first_error.has_value()) return;

            const auto key_sv = key.as_str();
            auto converted = parse_setting_value(key_sv, value);
            if (!converted) {
                first_error = converted.error();
                return;
            }

            auto result = set(key_sv, std::move(*converted));
            if (!result) {
                first_error = result.error();
            }
        });

        if (first_error) {
            return std::unexpected(*first_error);
        }
        return {};
    }

    // 获取所有已注册的 schema（用于 UI 生成）
    [[nodiscard]] auto schemas() const
        -> const std::map<std::string, SettingSchema>& {
        return state_.schemas;
    }

    // 获取所有 key 列表
    [[nodiscard]] auto all_keys() const -> std::vector<std::string> {
        std::vector<std::string> keys;
        keys.reserve(state_.schemas.size());
        for (const auto& [k, _] : state_.schemas) {
            keys.push_back(k);
        }
        return keys;
    }

private:
    SettingsHookState state_;
    std::uint64_t next_sub_id_{0};
    std::map<std::string, std::vector<std::pair<std::uint64_t, SettingChangeCallback>>> subscribers_;

    // 在指定层级写入值
    auto set_at_layer(std::string key, SettingLayer layer, SettingValue value) -> void {
        auto& entries = state_.entries[key];
        // 移除同层旧值
        std::erase_if(entries, [layer](const auto& e) { return e.layer == layer; });
        entries.push_back(LayeredEntry{.layer = layer, .value = std::move(value)});
    }

    // 获取有效值（最高优先级层的值）
    [[nodiscard]] auto get_effective_value(std::string_view key) const
        -> std::optional<SettingValue> {
        auto it = state_.entries.find(std::string(key));
        if (it == state_.entries.end() || it->second.empty()) return std::nullopt;
        // 层级枚举值越大优先级越高
        auto max_it = std::max_element(it->second.begin(), it->second.end(),
            [](const auto& a, const auto& b) {
                return static_cast<int>(a.layer) < static_cast<int>(b.layer);
            });
        return max_it->value;
    }

    // 通知订阅者
    auto notify_subscribers(std::string_view key) -> void {
        auto it = subscribers_.find(std::string(key));
        if (it == subscribers_.end()) return;
        auto effective = get_effective_value(key);
        if (!effective.has_value()) return;
        for (const auto& [_, cb] : it->second) {
            if (cb) cb(key, *effective);
        }
    }

    // 类型匹配检查
    [[nodiscard]] static auto type_matches(SettingType expected, const SettingValue& value)
        -> bool {
        switch (expected) {
            case SettingType::Bool:        return std::holds_alternative<bool>(value);
            case SettingType::Int:         return std::holds_alternative<int64_t>(value);
            case SettingType::Float:       return std::holds_alternative<double>(value);
            case SettingType::String:      return std::holds_alternative<std::string>(value);
            case SettingType::StringArray: return std::holds_alternative<std::vector<std::string>>(value);
        }
        return false;
    }

    // 类型名称字符串
    [[nodiscard]] static auto type_name(SettingType t) -> std::string_view {
        switch (t) {
            case SettingType::Bool:        return "bool";
            case SettingType::Int:         return "int";
            case SettingType::Float:       return "float";
            case SettingType::String:      return "string";
            case SettingType::StringArray: return "string[]";
        }
        return "unknown";
    }

    // 简易值格式化（用于导出）
    [[nodiscard]] static auto format_value(const SettingValue& value) -> std::string {
        return std::visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return std::format("{}", v);
            } else if constexpr (std::is_same_v<T, double>) {
                return std::format("{}", v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return std::format("\"{}\"", v);
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                std::string arr = "[";
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (i > 0) arr += ", ";
                    arr += std::format("\"{}\"", v[i]);
                }
                arr += "]";
                return arr;
            }
            return "null";
        }, value);
    }

    [[nodiscard]] static auto make_json_value(cc::utils::json::JsonMutDoc& doc, const SettingValue& value)
        -> cc::utils::json::JsonMutVal {
        return std::visit([&doc](const auto& v) -> cc::utils::json::JsonMutVal {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return doc.boolean(v);
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return doc.number(v);
            } else if constexpr (std::is_same_v<T, double>) {
                return doc.number(v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return doc.string(v);
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                auto arr = doc.array();
                for (const auto& item : v) {
                    arr.append(doc.string(item));
                }
                return arr;
            }
            return doc.null();
        }, value);
    }

    [[nodiscard]] auto parse_setting_value(std::string_view key, cc::utils::json::JsonVal value) const
        -> std::expected<SettingValue, std::string> {
        auto schema_it = state_.schemas.find(std::string(key));
        const auto expected = schema_it != state_.schemas.end()
            ? std::optional<SettingType>(schema_it->second.type)
            : std::nullopt;

        if (value.is_bool()) {
            if (expected && *expected != SettingType::Bool) return type_error(key, *expected);
            return SettingValue{value.as_bool()};
        }
        if (value.is_str()) {
            if (expected && *expected != SettingType::String) return type_error(key, *expected);
            return SettingValue{std::string(value.as_str())};
        }
        if (value.is_num()) {
            if (expected == SettingType::Int) return SettingValue{value.as_int()};
            if (expected == SettingType::Float || !expected) return SettingValue{value.as_double()};
            return type_error(key, *expected);
        }
        if (value.is_arr()) {
            if (expected && *expected != SettingType::StringArray) return type_error(key, *expected);
            std::vector<std::string> strings;
            strings.reserve(value.size());
            std::optional<std::string> err;
            value.iter([&](cc::utils::json::JsonVal item) {
                if (err) return;
                if (!item.is_str()) {
                    err = std::format("Setting '{}' expects an array of strings", key);
                    return;
                }
                strings.emplace_back(item.as_str());
            });
            if (err) return std::unexpected(*err);
            return SettingValue{std::move(strings)};
        }
        return std::unexpected(std::format("Unsupported JSON value for setting '{}'", key));
    }

    [[nodiscard]] static auto type_error(std::string_view key, SettingType expected)
        -> std::expected<SettingValue, std::string> {
        return std::unexpected(std::format("Type mismatch for '{}': expected {}", key, type_name(expected)));
    }
};

} // namespace cc::hooks
