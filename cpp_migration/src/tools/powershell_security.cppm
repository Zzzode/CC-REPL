module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <algorithm>

export module cc.tools.powershell_security;

import cc.tools.bash_security;

export namespace cc::tools {

// 获取被阻止的 PowerShell cmdlet 列表
inline auto get_blocked_cmdlets() -> std::vector<std::string> {
    return {
        "Format-Volume",
        "Clear-Disk",
        "Remove-Partition",
        "Initialize-Disk",
        "Set-ExecutionPolicy Unrestricted",
        "Invoke-Expression",  // iex - 动态执行风险
        "Start-Process -Verb RunAs",  // 提权
        "New-Service",
        "Remove-Item -Recurse -Force /",
        "Remove-Item -Recurse -Force C:\\",
        "Stop-Computer",
        "Restart-Computer",
        "Disable-NetAdapter",
        "Clear-EventLog"
    };
}

// 判断是否为危险的 cmdlet
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
        "ConvertTo-SecureString"  // 可能用于密码操作
    };

    return std::any_of(dangerous_cmdlets.begin(), dangerous_cmdlets.end(),
        [&](std::string_view cmdlet) {
            return command.find(cmdlet) != std::string_view::npos;
        });
}

// 检测 PowerShell 特有的注入模式
inline auto detect_ps_injection(std::string_view command) -> bool {
    static const std::vector<std::string_view> ps_injection_patterns = {
        "$(",               // 子表达式
        "Invoke-Expression",
        "iex ",
        "iex(",
        "[ScriptBlock]::Create",
        "Add-Type",         // 动态编译 C# 代码
        "New-Object System.Net.WebClient",
        "DownloadString",
        "DownloadFile",
        "-EncodedCommand",  // Base64 编码绕过
        "-enc ",
        "FromBase64String",
        "[Convert]::FromBase64",
        "Invoke-WebRequest", // 与 -OutFile 组合可能下载恶意脚本
    };

    return std::any_of(ps_injection_patterns.begin(), ps_injection_patterns.end(),
        [&](std::string_view pattern) {
            return command.find(pattern) != std::string_view::npos;
        });
}

// PowerShell 命令综合安全检查
inline auto check_ps_security(std::string_view command) -> SecurityCheck {
    if (command.empty()) {
        return SecurityCheck{false, "Empty PowerShell command", std::nullopt};
    }

    // 检查阻止列表
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

    // 检查危险 cmdlet
    if (is_dangerous_cmdlet(command)) {
        return SecurityCheck{
            false,
            "Dangerous PowerShell cmdlet detected",
            "Review the command carefully and consider safer alternatives"
        };
    }

    // 检查注入
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
