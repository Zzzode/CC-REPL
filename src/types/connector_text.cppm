/// @file connector_text.cppm
/// @brief Connector text block types and type-guard utility.
/// Migrated from: src/types/connectorText.ts
module;

#include <string>
#include <string_view>
#include <variant>

export module cc.types.connector_text;

export namespace cc::types::connector_text {

/// Type discriminator string constants
inline constexpr std::string_view kConnectorTextType = "connector_text";
inline constexpr std::string_view kConnectorTextDeltaType = "connector_text_delta";

/// A complete connector text content block
struct ConnectorTextBlock {
    std::string type = std::string(kConnectorTextType);
    std::string connector_text;
};

/// An incremental connector text delta block
struct ConnectorTextDelta {
    std::string type = std::string(kConnectorTextDeltaType);
    std::string connector_text_delta;
};

/// Union of connector text block types
using ConnectorTextVariant = std::variant<ConnectorTextBlock, ConnectorTextDelta>;

/// Type guard: check whether a ConnectorTextVariant holds a ConnectorTextBlock
[[nodiscard]] inline bool is_connector_text_block(const ConnectorTextVariant& value) {
    if (auto* block = std::get_if<ConnectorTextBlock>(&value)) {
        return block->type == kConnectorTextType && !block->connector_text.empty();
    }
    return false;
}

} // namespace cc::types::connector_text
