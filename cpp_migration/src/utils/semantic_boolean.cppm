module;

#include <optional>
#include <string_view>

export module cc.utils.semantic_boolean;

export namespace cc::utils::semantic_boolean {

[[nodiscard]] inline std::optional<bool> coerce_semantic_boolean(std::string_view value) noexcept {
    if (value == "true") return true;
    if (value == "false") return false;
    return std::nullopt;
}

} // namespace cc::utils::semantic_boolean
