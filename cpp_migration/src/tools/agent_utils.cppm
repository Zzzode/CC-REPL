module;
#include <string>
#include <string_view>
#include <sstream>
#include <random>
#include <chrono>
#include <iomanip>
#include <array>

export module cc.tools.agent_utils;

export namespace cc::tools {

// 生成唯一的代理 ID（格式：agent_<timestamp_hex>_<random>）
inline auto generate_agent_id() -> std::string {
    auto now = std::chrono::system_clock::now();
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    // 生成 4 位随机十六进制后缀
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0x1000, 0xFFFF);

    std::ostringstream oss;
    oss << "agent_" << std::hex << epoch_ms << "_" << dist(gen);
    return oss.str();
}

// 根据代理索引返回 ANSI 颜色码（用于终端输出区分多个代理）
inline auto get_agent_color(int index) -> std::string {
    // 使用一组容易区分的颜色循环分配
    static const std::array<std::string_view, 8> colors = {
        "\033[36m",  // Cyan
        "\033[33m",  // Yellow
        "\033[35m",  // Magenta
        "\033[32m",  // Green
        "\033[34m",  // Blue
        "\033[91m",  // Bright Red
        "\033[96m",  // Bright Cyan
        "\033[93m",  // Bright Yellow
    };
    return std::string(colors[index % colors.size()]);
}

// 格式化代理状态显示字符串
inline auto format_agent_status(
    std::string_view agent_name,
    std::string_view status
) -> std::string {
    std::ostringstream oss;
    oss << "[" << agent_name << "] " << status;
    return oss.str();
}

// 启发式判断是否应该派生子代理来处理任务
inline auto should_fork_agent(std::string_view task) -> bool {
    // 任务长度过短通常不值得启动子代理
    if (task.size() < 50) {
        return false;
    }

    // 包含明确的并行/独立子任务关键词时建议派生
    static const std::array<std::string_view, 8> fork_indicators = {
        "in parallel",
        "simultaneously",
        "independently",
        "at the same time",
        "multiple files",
        "each directory",
        "all of the following",
        "concurrently"
    };

    for (const auto& indicator : fork_indicators) {
        if (task.find(indicator) != std::string_view::npos) {
            return true;
        }
    }

    // 任务描述非常长时也建议派生（可能包含多个子任务）
    if (task.size() > 500) {
        return true;
    }

    return false;
}

} // namespace cc::tools
