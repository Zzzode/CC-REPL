module;

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

export module cc.utils.binary_check;

export namespace cc::utils::binary_check {

[[nodiscard]] inline std::string trim_command(std::string_view command) {
    const auto start = command.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string_view::npos) return {};
    const auto end = command.find_last_not_of(" \t\n\r\f\v");
    return std::string(command.substr(start, end - start + 1));
}

class BinaryChecker {
public:
    using Resolver = std::function<bool(std::string_view)>;

    [[nodiscard]] bool is_binary_installed(std::string_view command, const Resolver& resolver) {
        const auto trimmed = trim_command(command);
        if (trimmed.empty()) return false;
        if (auto it = cache_.find(trimmed); it != cache_.end()) return it->second;
        const bool exists = resolver ? resolver(trimmed) : false;
        cache_[trimmed] = exists;
        return exists;
    }

    void clear() noexcept {
        cache_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return cache_.size();
    }

private:
    std::unordered_map<std::string, bool> cache_;
};

} // namespace cc::utils::binary_check
