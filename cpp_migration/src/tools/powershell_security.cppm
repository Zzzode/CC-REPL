module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <algorithm>

export module cc.tools.powershell_security;

import cc.tools.bash_security;

export namespace cc::tools {


inline auto get_blocked_cmdlets() -> std::vector<std::string> {
    return {
        "Format-Volume",
        "Clear-Disk",
        "Remove-Partition",
        "Initialize-Disk",
        "Set-ExecutionPolicy Unrestricted",
        "Invoke-Expression",
        "Start-Process -Verb RunAs",
        "New-Service",
        "Remove-Item -Recurse -Force /",
        "Remove-Item -Recurse -Force C:\\",
        "Stop-Computer",
        "Restart-Computer",
        "Disable-NetAdapter",
        "Clear-EventLog"
    };
}


inline auto is_dangerous_cmdlet(std::string_view command) -> bool {
    static const std::vector<std::string_view> dangerous_cmdlets = {
        "Remove-Item -Recurse",
        "Format-Volume",
        "Clear-Disk",
        "Remove-Partition",
        "Stop-Computer",
        "Restart-Computer",
        "Invoke-Expression",
        "iex",
        "Set-ExecutionPolicy",
        "Disable-NetAdapter",
        "Stop-Service",
        "Remove-Service",
        "Clear-Content",
        "Clear-RecycleBin",
        "ConvertTo-SecureString"
    };

    return std::any_of(dangerous_cmdlets.begin(), dangerous_cmdlets.end(),
        [&](std::string_view cmdlet) {
            return command.find(cmdlet) != std::string_view::npos;
        });
}


inline auto detect_ps_injection(std::string_view command) -> bool {
    static const std::vector<std::string_view> ps_injection_patterns = {
        "$(",
        "Invoke-Expression",
        "iex ",
        "iex(",
        "[ScriptBlock]::Create",
        "Add-Type",
        "New-Object System.Net.WebClient",
        "DownloadString",
        "DownloadFile",
        "-EncodedCommand",
        "-enc ",
        "FromBase64String",
        "[Convert]::FromBase64",
        "Invoke-WebRequest",
    };

    return std::any_of(ps_injection_patterns.begin(), ps_injection_patterns.end(),
        [&](std::string_view pattern) {
            return command.find(pattern) != std::string_view::npos;
        });
}


inline auto check_ps_security(std::string_view command) -> SecurityCheck {
    if (command.empty()) {
        return SecurityCheck{false, "Empty PowerShell command", std::nullopt};
    }


    const auto blocked = get_blocked_cmdlets();
    for (const auto& blocked_cmd : blocked) {
        if (command.find(blocked_cmd) != std::string_view::npos) {
            return SecurityCheck{
                false,
                "Blocked PowerShell cmdlet: " + blocked_cmd,
                "Use a safer alternative cmdlet"
            };
        }
    }


    if (is_dangerous_cmdlet(command)) {
        return SecurityCheck{
            false,
            "Dangerous PowerShell cmdlet detected",
            "Review the command carefully and consider safer alternatives"
        };
    }


    if (detect_ps_injection(command)) {
        return SecurityCheck{
            false,
            "Potential PowerShell injection detected",
            "Avoid dynamic code execution and encoded commands"
        };
    }

    return SecurityCheck{true, "PowerShell command passed security checks", std::nullopt};
}

} // namespace cc::tools
