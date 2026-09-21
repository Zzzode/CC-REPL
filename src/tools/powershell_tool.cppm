// PowerShellTool - Windows PowerShell command execution with safety checks
module;
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

export module cc.tools.powershell;
import cc.utils.bash_execution;


export namespace cc::tools {


enum class PowerShellError {
    CommandEmpty,
    DangerousCmdlet,
    ExecutionFailed,
    Timeout,
    EncodingError,
    InvalidWorkingDirectory,
    NotAvailable,
};

constexpr auto format_error(PowerShellError err) -> std::string_view {
    switch (err) {
        case PowerShellError::CommandEmpty:           return "PowerShell command is empty";
        case PowerShellError::DangerousCmdlet:       return "Dangerous cmdlet detected, needs approval";
        case PowerShellError::ExecutionFailed:        return "PowerShell execution failed";
        case PowerShellError::Timeout:               return "PowerShell command timed out";
        case PowerShellError::EncodingError:         return "Output encoding conversion failed";
        case PowerShellError::InvalidWorkingDirectory: return "Working directory does not exist";
        case PowerShellError::NotAvailable:          return "PowerShell is not available on this system";
        default:                                     return "Unknown PowerShell error";
    }
}


inline constexpr std::array kDangerousCmdlets = {
    std::string_view{"Remove-Item -Recurse"},
    std::string_view{"Remove-Item -Force"},
    std::string_view{"Format-Volume"},
    std::string_view{"Format-Disk"},
    std::string_view{"Stop-Process"},
    std::string_view{"Stop-Computer"},
    std::string_view{"Restart-Computer"},
    std::string_view{"Clear-Disk"},
    std::string_view{"Initialize-Disk"},
    std::string_view{"Set-ExecutionPolicy Unrestricted"},
    std::string_view{"Invoke-Expression"},
    std::string_view{"Remove-Partition"},
};


enum class CmdletPermission {
    Allowed,
    NeedsApproval,
    Denied,
};


struct PowerShellConfig {
    std::string command;
    std::filesystem::path working_directory;
    std::chrono::seconds timeout{120};
    bool no_profile{true};
    bool non_interactive{true};
};


struct PowerShellResult {
    int exit_code{0};
    std::string stdout_output;
    std::string stderr_output;
    std::chrono::milliseconds duration{0};
    bool timed_out{false};
};

[[nodiscard]] inline std::string powershell_single_quote(std::string_view value) {
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

[[nodiscard]] inline std::string powershell_base64_encode(std::string_view bytes) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        const auto b0 = static_cast<unsigned char>(bytes[i++]);
        const auto b1 = static_cast<unsigned char>(bytes[i++]);
        const auto b2 = static_cast<unsigned char>(bytes[i++]);
        encoded.push_back(table[b0 >> 2]);
        encoded.push_back(table[((b0 & 0x03) << 4) | (b1 >> 4)]);
        encoded.push_back(table[((b1 & 0x0f) << 2) | (b2 >> 6)]);
        encoded.push_back(table[b2 & 0x3f]);
    }
    if (i < bytes.size()) {
        const auto b0 = static_cast<unsigned char>(bytes[i++]);
        encoded.push_back(table[b0 >> 2]);
        if (i < bytes.size()) {
            const auto b1 = static_cast<unsigned char>(bytes[i++]);
            encoded.push_back(table[((b0 & 0x03) << 4) | (b1 >> 4)]);
            encoded.push_back(table[(b1 & 0x0f) << 2]);
            encoded.push_back('=');
        } else {
            encoded.push_back(table[(b0 & 0x03) << 4]);
            encoded.push_back('=');
            encoded.push_back('=');
        }
    }
    return encoded;
}

inline void append_powershell_utf16le(std::string& output, char32_t code_point) {
    auto append_unit = [&](std::uint16_t unit) {
        output.push_back(static_cast<char>(unit & 0xff));
        output.push_back(static_cast<char>((unit >> 8) & 0xff));
    };

    if (code_point > 0x10ffff) code_point = 0xfffd;
    if (code_point <= 0xffff) {
        if (code_point >= 0xd800 && code_point <= 0xdfff) code_point = 0xfffd;
        append_unit(static_cast<std::uint16_t>(code_point));
        return;
    }

    code_point -= 0x10000;
    append_unit(static_cast<std::uint16_t>(0xd800 + ((code_point >> 10) & 0x3ff)));
    append_unit(static_cast<std::uint16_t>(0xdc00 + (code_point & 0x3ff)));
}

[[nodiscard]] inline std::string powershell_utf8_to_utf16le(std::string_view value) {
    std::string utf16le;
    utf16le.reserve(value.size() * 2);
    for (std::size_t i = 0; i < value.size();) {
        const auto b0 = static_cast<unsigned char>(value[i]);
        char32_t code_point = 0xfffd;
        std::size_t consumed = 1;
        auto continuation = [&](std::size_t offset) -> std::optional<unsigned char> {
            if (i + offset >= value.size()) return std::nullopt;
            const auto byte = static_cast<unsigned char>(value[i + offset]);
            if ((byte & 0xc0) != 0x80) return std::nullopt;
            return byte;
        };

        if (b0 < 0x80) {
            code_point = b0;
        } else if ((b0 & 0xe0) == 0xc0) {
            if (auto b1 = continuation(1)) {
                const auto candidate = static_cast<char32_t>(((b0 & 0x1f) << 6) | (*b1 & 0x3f));
                if (candidate >= 0x80) {
                    code_point = candidate;
                    consumed = 2;
                }
            }
        } else if ((b0 & 0xf0) == 0xe0) {
            auto b1 = continuation(1);
            auto b2 = continuation(2);
            if (b1 && b2) {
                const auto candidate = static_cast<char32_t>(
                    ((b0 & 0x0f) << 12) | ((*b1 & 0x3f) << 6) | (*b2 & 0x3f)
                );
                if (candidate >= 0x800 && (candidate < 0xd800 || candidate > 0xdfff)) {
                    code_point = candidate;
                    consumed = 3;
                }
            }
        } else if ((b0 & 0xf8) == 0xf0) {
            auto b1 = continuation(1);
            auto b2 = continuation(2);
            auto b3 = continuation(3);
            if (b1 && b2 && b3) {
                const auto candidate = static_cast<char32_t>(
                    ((b0 & 0x07) << 18) | ((*b1 & 0x3f) << 12) |
                    ((*b2 & 0x3f) << 6) | (*b3 & 0x3f)
                );
                if (candidate >= 0x10000 && candidate <= 0x10ffff) {
                    code_point = candidate;
                    consumed = 4;
                }
            }
        }

        append_powershell_utf16le(utf16le, code_point);
        i += consumed;
    }
    return utf16le;
}

[[nodiscard]] inline std::string powershell_encoded_command(std::string_view script) {
    return powershell_base64_encode(powershell_utf8_to_utf16le(script));
}

[[nodiscard]] inline std::string powershell_path_string(const std::filesystem::path& path) {
    const auto utf8_path = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size());
}

[[nodiscard]] inline std::string build_powershell_script(
    const PowerShellConfig& config,
    const std::filesystem::path& default_cwd
) {
    auto cwd = config.working_directory.empty() ? default_cwd : config.working_directory;
    return "Set-Location -LiteralPath " + powershell_single_quote(powershell_path_string(cwd)) + "\n" + config.command;
}

[[nodiscard]] inline std::string build_powershell_process_command(
    const PowerShellConfig& config,
    const std::filesystem::path& default_cwd = std::filesystem::current_path()
) {
    std::string command = "powershell.exe";
    if (config.no_profile) command += " -NoProfile";
    if (config.non_interactive) command += " -NonInteractive";
    command += " -EncodedCommand " + powershell_encoded_command(build_powershell_script(config, default_cwd));
    return command;
}


class EncodingHandler {
public:

    static auto utf16le_to_utf8(std::span<const std::byte> input) -> std::expected<std::string, PowerShellError> {
        if (input.empty()) return std::string{};


        if (input.size() % 2 != 0) {
            return std::unexpected(PowerShellError::EncodingError);
        }

        std::string result;
        result.reserve(input.size());

        for (size_t i = 0; i + 1 < input.size(); i += 2) {

            uint16_t code_unit = static_cast<uint16_t>(input[i]) |
                                 (static_cast<uint16_t>(input[i + 1]) << 8);


            if (code_unit < 0x80) {
                result += static_cast<char>(code_unit);
            } else if (code_unit < 0x800) {
                result += static_cast<char>(0xC0 | (code_unit >> 6));
                result += static_cast<char>(0x80 | (code_unit & 0x3F));
            } else {
                result += static_cast<char>(0xE0 | (code_unit >> 12));
                result += static_cast<char>(0x80 | ((code_unit >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (code_unit & 0x3F));
            }
        }
        return result;
    }


    static auto has_utf16_bom(std::span<const std::byte> data) -> bool {
        return data.size() >= 2 &&
               data[0] == std::byte{0xFF} && data[1] == std::byte{0xFE};
    }
};


class PowerShellTool {
public:
    static constexpr std::string_view name = "powershell";
    static constexpr std::string_view description = "Execute PowerShell commands on Windows systems";

    explicit PowerShellTool(std::filesystem::path default_cwd = std::filesystem::current_path())
        : default_cwd_(std::move(default_cwd)) {}


    auto check_permission(std::string_view command) const -> CmdletPermission {
        if (command.empty()) return CmdletPermission::Denied;

        for (auto pattern : kDangerousCmdlets) {
            if (command.find(pattern) != std::string_view::npos) {
                return CmdletPermission::NeedsApproval;
            }
        }
        return CmdletPermission::Allowed;
    }


    auto validate(const PowerShellConfig& config) const -> std::expected<void, PowerShellError> {
        if (config.command.empty()) {
            return std::unexpected(PowerShellError::CommandEmpty);
        }

        auto cwd = config.working_directory.empty() ? default_cwd_ : config.working_directory;
        if (!std::filesystem::exists(cwd)) {
            return std::unexpected(PowerShellError::InvalidWorkingDirectory);
        }

        if (check_permission(config.command) != CmdletPermission::Allowed) {
            return std::unexpected(PowerShellError::DangerousCmdlet);
        }
        return {};
    }


    auto execute(PowerShellConfig config) -> std::expected<PowerShellResult, PowerShellError> {
        if (auto v = validate(config); !v) return std::unexpected(v.error());

        auto start_time = std::chrono::steady_clock::now();


        const auto ps_cmd = build_powershell_process_command(config, default_cwd_);


        auto pipe_cap = cc::utils::bash::exec_capture(ps_cmd.c_str());
    if (!pipe_cap) return std::unexpected(PowerShellError::ExecutionFailed);
    std::string output = std::move(pipe_cap->output);
    auto status = pipe_cap->status;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);

        return PowerShellResult{
            .exit_code = status,
            .stdout_output = std::move(output),
            .stderr_output = {},
            .duration = elapsed,
            .timed_out = false,
        };
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "command": {{ "type": "string", "description": "PowerShell command to execute" }},
      "cwd": {{ "type": "string", "description": "Working directory for execution" }},
      "timeout": {{ "type": "integer", "description": "Timeout in seconds (default 120)" }}
    }},
    "required": ["command"]
  }}
}})json", name, description);
    }

private:
    std::filesystem::path default_cwd_;
};

} // namespace cc::tools
