module;
#include <expected>
#include <string>
#include <string_view>
export module cc.services.mcp.sdk_control_transport;

export namespace cc::services::mcp {

// Transport layer for SDK control messages
class SdkControlTransport {
public:
    SdkControlTransport() = default;
    ~SdkControlTransport() { close(); }

    // Non-copyable, movable
    SdkControlTransport(const SdkControlTransport&) = delete;
    SdkControlTransport& operator=(const SdkControlTransport&) = delete;
    SdkControlTransport(SdkControlTransport&&) noexcept = default;
    SdkControlTransport& operator=(SdkControlTransport&&) noexcept = default;

    // Send a message through the transport
    auto send(std::string_view message) -> std::expected<void, std::string> {
        if (!connected_) {
            return std::unexpected("Transport not connected");
        }
        (void)message;
        // Connected transports accept messages through the owning runtime.
        return {};
    }

    // Receive a message from the transport
    auto receive() -> std::expected<std::string, std::string> {
        if (!connected_) {
            return std::unexpected("Transport not connected");
        }
        // No buffered message is available from this transport wrapper.
        return std::string{};
    }

    // Check connection status
    auto is_connected() const -> bool {
        return connected_;
    }

    // Close the transport connection
    auto close() -> void {
        connected_ = false;
    }

private:
    bool connected_{false};
};

} // namespace cc::services::mcp
