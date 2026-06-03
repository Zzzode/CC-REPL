module;
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cstdlib>

export module cc.tools.powershell_permissions;

import cc.tools.bash_permissions;

export namespace cc::tools {


inline auto is_read_only_cmdlet(std::string_view command) -> bool {
    static const std::vector<std::string_view> read_only_prefixes = {
        "Get-",
        "Measure-",
        "Test-",
        "Select-",
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


    auto trimmed = command;
    while (!trimmed.empty() && trimmed.front() == ' ') {
        trimmed.remove_prefix(1);
    }

    return std::any_of(read_only_prefixes.begin(), read_only_prefixes.end(),
        [&](std::string_view prefix) {
            return trimmed.starts_with(prefix);
        });
}


inline auto get_ps_execution_policy() -> std::string {


    #ifdef _WIN32

    return "RemoteSigned";
    #else

    return "Unrestricted";
    #endif
}


inline auto check_ps_permission(
    std::string_view command,
    PermissionMode mode
) -> BashPermissionLevel {

    if (command.empty()) {
        return BashPermissionLevel::Blocked;
    }


    static const std::vector<std::string_view> blocked_patterns = {
        "Format-Volume",
        "Clear-Disk",
        "Remove-Partition",
        "Stop-Computer",
        "Restart-Computer",
        "-Verb RunAs",
        "Set-ExecutionPolicy Unrestricted"
    };

    for (const auto& pattern : blocked_patterns) {
        if (command.find(pattern) != std::string_view::npos) {
            return BashPermissionLevel::Blocked;
        }
    }


    if (mode == PermissionMode::Permissive) {
        return BashPermissionLevel::Allowed;
    }


    if (is_read_only_cmdlet(command)) {
        return BashPermissionLevel::Allowed;
    }


    if (mode == PermissionMode::Strict) {
        return BashPermissionLevel::NeedsApproval;
    }


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
