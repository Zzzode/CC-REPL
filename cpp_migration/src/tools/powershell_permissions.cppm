module;
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cstdlib>

export module cc.tools.powershell_permissions;

import cc.tools.bash_permissions;

export namespace cc::tools {

// 判断是否为只读 cmdlet（不会修改系统状态）
inline auto is_read_only_cmdlet(std::string_view command) -> bool {
    static const std::vector<std::string_view> read_only_prefixes = {
        "Get-",           // 所有 Get- 命令均为只读
        "Measure-",       // 度量命令
        "Test-",          // 测试命令
        "Select-",        // 选择/过滤
        "Where-Object",
        "Format-Table",
        "Format-List",
        "Out-String",
        "ConvertTo-Json",
        "ConvertFrom-Json",
        "Write-Host",
        "Write-Output",
        "Read-Host",
        "Compare-Object",
        "Sort-Object",
        "Group-Object",
    };

    // 提取命令主体（去除前导空格）
    auto trimmed = command;
    while (!trimmed.empty() && trimmed.front() == ' ') {
        trimmed.remove_prefix(1);
    }

    return std::any_of(read_only_prefixes.begin(), read_only_prefixes.end(),
        [&](std::string_view prefix) {
            return trimmed.starts_with(prefix);
        });
}

// 获取当前系统的 PowerShell 执行策略
inline auto get_ps_execution_policy() -> std::string {
    // 在实际环境中应调用 PowerShell 获取策略
    // 此处返回默认值，生产代码需通过进程调用实现
    #ifdef _WIN32
    // Windows 上执行 powershell -c "Get-ExecutionPolicy"
    return "RemoteSigned"; // Windows 默认策略
    #else
    // macOS/Linux 上 PowerShell Core 默认为 Unrestricted
    return "Unrestricted";
    #endif
}

// PowerShell 命令权限检查
inline auto check_ps_permission(
    std::string_view command,
    PermissionMode mode
) -> BashPermissionLevel {
    // 空命令阻止
    if (command.empty()) {
        return BashPermissionLevel::Blocked;
    }

    // 检查已知阻止的 cmdlet 模式
    static const std::vector<std::string_view> blocked_patterns = {
        "Format-Volume",
        "Clear-Disk",
        "Remove-Partition",
        "Stop-Computer",
        "Restart-Computer",
        "-Verb RunAs",     // 提权运行
        "Set-ExecutionPolicy Unrestricted"
    };

    for (const auto& pattern : blocked_patterns) {
        if (command.find(pattern) != std::string_view::npos) {
            return BashPermissionLevel::Blocked;
        }
    }

    // 宽松模式下非阻止命令一律允许
    if (mode == PermissionMode::Permissive) {
        return BashPermissionLevel::Allowed;
    }

    // 只读 cmdlet 在所有模式下允许
    if (is_read_only_cmdlet(command)) {
        return BashPermissionLevel::Allowed;
    }

    // 严格模式下非只读 cmdlet 需要审批
    if (mode == PermissionMode::Strict) {
        return BashPermissionLevel::NeedsApproval;
    }

    // 普通模式下，修改性 cmdlet 需要审批
    static const std::vector<std::string_view> write_prefixes = {
        "Set-", "New-", "Remove-", "Add-", "Clear-",
        "Enable-", "Disable-", "Start-", "Stop-",
        "Rename-", "Move-", "Copy-", "Install-",
        "Uninstall-", "Update-", "Register-", "Invoke-"
    };

    auto trimmed = command;
    while (!trimmed.empty() && trimmed.front() == ' ') {
        trimmed.remove_prefix(1);
    }

    for (const auto& prefix : write_prefixes) {
        if (trimmed.starts_with(prefix)) {
            return BashPermissionLevel::NeedsApproval;
        }
    }

    return BashPermissionLevel::Allowed;
}

} // namespace cc::tools
