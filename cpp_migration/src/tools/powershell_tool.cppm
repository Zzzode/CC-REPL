// PowerShellTool - Windows PowerShell command execution with safety checks
module;
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

export module cc.tools.powershell;


export namespace cc::tools {

// PowerShell 执行错误类型
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

// 危险 cmdlet 模式列表
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

// 命令权限级别
enum class CmdletPermission {
    Allowed,
    NeedsApproval,
    Denied,
};

// PowerShell 执行配置
struct PowerShellConfig {
    std::string command;
    std::filesystem::path working_directory;
    std::chrono::seconds timeout{120};
    bool no_profile{true};        // 是否使用 -NoProfile 启动
    bool non_interactive{true};   // -NonInteractive 模式
};

// PowerShell 执行结果
struct PowerShellResult {
    int exit_code{0};
    std::string stdout_output;
    std::string stderr_output;
    std::chrono::milliseconds duration{0};
    bool timed_out{false};
};

// UTF-16 编码处理工具
class EncodingHandler {
public:
    // 将 UTF-16LE 字节流转换为 UTF-8 字符串
    static auto utf16le_to_utf8(std::span<const std::byte> input) -> std::expected<std::string, PowerShellError> {
        if (input.empty()) return std::string{};

        // 确保字节数为偶数 (UTF-16 每个码元 2 字节)
        if (input.size() % 2 != 0) {
            return std::unexpected(PowerShellError::EncodingError);
        }

        std::string result;
        result.reserve(input.size()); // UTF-8 通常不比 UTF-16 大多少

        for (size_t i = 0; i + 1 < input.size(); i += 2) {
            // 小端序读取 UTF-16 码元
            uint16_t code_unit = static_cast<uint16_t>(input[i]) |
                                 (static_cast<uint16_t>(input[i + 1]) << 8);

            // BMP 字符直接编码为 UTF-8
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

    // 检测是否含有 UTF-16 BOM
    static auto has_utf16_bom(std::span<const std::byte> data) -> bool {
        return data.size() >= 2 &&
               data[0] == std::byte{0xFF} && data[1] == std::byte{0xFE};
    }
};

// PowerShellTool - Windows PowerShell 命令执行
class PowerShellTool {
public:
    static constexpr std::string_view name = "powershell";
    static constexpr std::string_view description = "Execute PowerShell commands on Windows systems";

    explicit PowerShellTool(std::filesystem::path default_cwd = std::filesystem::current_path())
        : default_cwd_(std::move(default_cwd)) {}

    // 检测危险 cmdlet
    auto check_permission(std::string_view command) const -> CmdletPermission {
        if (command.empty()) return CmdletPermission::Denied;

        for (auto pattern : kDangerousCmdlets) {
            if (command.find(pattern) != std::string_view::npos) {
                return CmdletPermission::NeedsApproval;
            }
        }
        return CmdletPermission::Allowed;
    }

    // 验证命令配置
    auto validate(const PowerShellConfig& config) const -> std::expected<void, PowerShellError> {
        if (config.command.empty()) {
            return std::unexpected(PowerShellError::CommandEmpty);
        }

        auto cwd = config.working_directory.empty() ? default_cwd_ : config.working_directory;
        if (!std::filesystem::exists(cwd)) {
            return std::unexpected(PowerShellError::InvalidWorkingDirectory);
        }

        if (check_permission(config.command) == CmdletPermission::Denied) {
            return std::unexpected(PowerShellError::DangerousCmdlet);
        }
        return {};
    }

    // 执行 PowerShell 命令
    auto execute(PowerShellConfig config) -> std::expected<PowerShellResult, PowerShellError> {
        if (auto v = validate(config); !v) return std::unexpected(v.error());

        auto start_time = std::chrono::steady_clock::now();

        // 构建 PowerShell 执行命令行
        std::string ps_cmd = "powershell.exe";
        if (config.no_profile) ps_cmd += " -NoProfile";
        if (config.non_interactive) ps_cmd += " -NonInteractive";
        ps_cmd += std::format(" -Command \"{}\"", config.command);

        // 通过 popen 执行 (跨平台占位实现)
        FILE* pipe = ::popen(ps_cmd.c_str(), "r");
        if (!pipe) return std::unexpected(PowerShellError::ExecutionFailed);

        std::string output;
        std::array<char, 4096> buffer{};
        while (auto bytes = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
            output.append(buffer.data(), bytes);
        }
        int status = ::pclose(pipe);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);

        return PowerShellResult{
            .exit_code = status,
            .stdout_output = std::move(output),
            .duration = elapsed,
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
