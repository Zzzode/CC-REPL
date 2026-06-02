module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <algorithm>

export module cc.commands.plugin.browse_marketplace;

export namespace cc::commands {

// 插件市场条目
struct MarketplaceEntry {
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    int downloads;
    float rating;
};

auto bundled_marketplace_entries() -> std::vector<MarketplaceEntry>;

// 浏览插件市场，可按分类筛选
auto browse_marketplace(std::optional<std::string> category = std::nullopt)
    -> std::expected<std::vector<MarketplaceEntry>, std::string> {
    auto entries = bundled_marketplace_entries();
    if (category) {
        std::erase_if(entries, [&](const auto& entry) {
            return entry.description.find(*category) == std::string::npos && entry.id.find(*category) == std::string::npos;
        });
    }
    return entries;
}

// 按关键词搜索插件市场
auto search_marketplace(std::string_view query)
    -> std::expected<std::vector<MarketplaceEntry>, std::string> {
    if (query.empty()) {
        return std::unexpected("Search query cannot be empty");
    }
    auto entries = bundled_marketplace_entries();
    std::erase_if(entries, [&](const auto& entry) {
        return entry.id.find(query) == std::string::npos && entry.name.find(query) == std::string::npos &&
               entry.description.find(query) == std::string::npos;
    });
    return entries;
}

auto bundled_marketplace_entries() -> std::vector<MarketplaceEntry> {
    return {
        {.id = "formatter-basic", .name = "Basic Formatter", .description = "formatters utilities", .author = "CC-REPL", .downloads = 0, .rating = 0.0F},
        {.id = "theme-classic", .name = "Classic Theme", .description = "themes", .author = "CC-REPL", .downloads = 0, .rating = 0.0F},
        {.id = "toolkit-local", .name = "Local Toolkit", .description = "tools integrations utilities", .author = "CC-REPL", .downloads = 0, .rating = 0.0F},
    };
}

// 获取所有可用的插件分类
auto get_categories() -> std::vector<std::string> {
    return {
        "tools",
        "formatters",
        "linters",
        "themes",
        "languages",
        "integrations",
        "utilities"
    };
}

} // namespace cc::commands
