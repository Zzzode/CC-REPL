/// @file bridge_debug.cppm
/// @brief Bridge fault injection framework for testing (ant-only infrastructure)

module;

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <algorithm>
#include <cstdlib>
#include <expected>
#include <sstream>

export module cc.bridge.bridge_debug;

export namespace cc::bridge {

/// Abstract handle that controls which bridge methods should have faults
/// injected and what error message those faults return.
class BridgeDebugHandle {
public:
    virtual ~BridgeDebugHandle() = default;

    /// Returns true if a fault should be injected for the given method name.
    [[nodiscard]] virtual auto should_inject_fault(std::string_view method) const
        -> bool = 0;

    /// Returns the error message to inject for the given method, or
    /// std::nullopt if no fault is configured.
    [[nodiscard]] virtual auto get_injected_error(std::string_view method) const
        -> std::optional<std::string> = 0;
};

/// Concrete debug handle driven by environment variables:
///   CC_BRIDGE_FAULT_METHODS  — comma-separated method names to fault
///   CC_BRIDGE_FAULT_ERROR    — the error message to inject
class EnvBridgeDebugHandle final : public BridgeDebugHandle {
public:
    EnvBridgeDebugHandle()
        : fault_methods_(parse_env_methods())
        , fault_error_(parse_env_error())
    {}

    [[nodiscard]] auto should_inject_fault(std::string_view method) const
        -> bool override
    {
        if (fault_methods_.empty()) return false;
        return std::any_of(fault_methods_.begin(),
                           fault_methods_.end(),
                           [&](const std::string& m) {
                               return m == method;
                           });
    }

    [[nodiscard]] auto get_injected_error(std::string_view method) const
        -> std::optional<std::string> override
    {
        if (!should_inject_fault(method)) return std::nullopt;
        return fault_error_.value_or("Injected bridge fault");
    }

private:
    std::vector<std::string> fault_methods_;
    std::optional<std::string> fault_error_;

    static auto parse_env_methods() -> std::vector<std::string> {
        const char* env = std::getenv("CC_BRIDGE_FAULT_METHODS");
        if (!env || env[0] == '\0') return {};

        std::vector<std::string> methods;
        std::istringstream stream(env);
        std::string token;
        while (std::getline(stream, token, ',')) {
            // Trim whitespace
            auto start = token.find_first_not_of(" \t");
            auto end   = token.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                methods.push_back(token.substr(start, end - start + 1));
            }
        }
        return methods;
    }

    static auto parse_env_error() -> std::optional<std::string> {
        const char* env = std::getenv("CC_BRIDGE_FAULT_ERROR");
        if (env && env[0] != '\0') {
            return std::string(env);
        }
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// Global handle management
// ---------------------------------------------------------------------------

namespace detail {

inline auto get_handle_mutex() -> std::mutex& {
    static std::mutex mtx;
    return mtx;
}

inline auto get_handle_storage() -> std::shared_ptr<BridgeDebugHandle>& {
    static std::shared_ptr<BridgeDebugHandle> handle;
    return handle;
}

} // namespace detail

/// Registers a debug handle that will be consulted by the fault injection
/// wrapper.  Replaces any previously registered handle.
inline void register_bridge_debug_handle(
    std::shared_ptr<BridgeDebugHandle> handle)
{
    std::lock_guard lock(detail::get_handle_mutex());
    detail::get_handle_storage() = std::move(handle);
}

/// Clears the currently registered debug handle, disabling fault injection.
inline void clear_bridge_debug_handle() {
    std::lock_guard lock(detail::get_handle_mutex());
    detail::get_handle_storage().reset();
}

/// Returns the currently registered debug handle, or nullptr if none is set.
inline auto get_bridge_debug_handle() -> std::shared_ptr<BridgeDebugHandle> {
    std::lock_guard lock(detail::get_handle_mutex());
    return detail::get_handle_storage();
}

// ---------------------------------------------------------------------------
// Fault-injection wrapper
// ---------------------------------------------------------------------------

/// Wraps an API function so that it consults the registered
/// BridgeDebugHandle before invoking the real implementation.  If the
/// handle indicates a fault should be injected for `method`, the wrapper
/// returns an error result instead of calling `api_fn`.
///
/// `api_fn` is expected to return `std::expected<T, std::string>`.
template <typename Fn>
auto wrap_api_for_fault_injection(std::string_view method, Fn&& api_fn)
    -> decltype(api_fn())
{
    if (auto handle = get_bridge_debug_handle()) {
        if (handle->should_inject_fault(method)) {
            using ResultType = decltype(api_fn());
            auto error_msg = handle->get_injected_error(method)
                                 .value_or("Injected bridge fault");
            return ResultType(std::unexpected(std::move(error_msg)));
        }
    }
    return std::forward<Fn>(api_fn)();
}

} // namespace cc::bridge
