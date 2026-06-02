module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <expected>
#include <optional>

export module cc.tools.file_edit_types;

export namespace cc::tools {

// 编辑操作的输入描述
struct EditOperation {
    std::filesystem::path file;  // 目标文件路径
    std::string old_text;        // 待替换的原文本
    std::string new_text;        // 替换后的新文本
    bool replace_all = false;    // 是否替换所有匹配项
};

// 编辑操作的执行结果
struct EditResult {
    bool success;                    // 操作是否成功
    int replacements;                // 实际替换次数
    std::vector<int> affected_lines; // 受影响的行号列表
    std::string preview;             // 替换后的上下文预览
};

// 编辑操作可能产生的错误类型
enum class EditError {
    FileNotFound,      // 文件不存在
    OldTextNotFound,   // 原文本在文件中未找到
    MultipleMatches,   // 原文本存在多处匹配但未指定 replace_all
    PermissionDenied,  // 文件无写权限
    BinaryFile         // 文件为二进制格式，不支持文本编辑
};

// 验证编辑操作的合法性
inline auto validate_edit(const EditOperation& op) -> std::expected<void, EditError> {
    namespace fs = std::filesystem;

    // 检查文件是否存在
    if (!fs::exists(op.file)) {
        return std::unexpected(EditError::FileNotFound);
    }

    // 检查文件权限
    auto perms = fs::status(op.file).permissions();
    if ((perms & fs::perms::owner_write) == fs::perms::none) {
        return std::unexpected(EditError::PermissionDenied);
    }

    // 简单的二进制文件检测（读取前几字节检查 null）
    // 这里仅做基础验证，实际实现会读取文件内容
    auto ext = op.file.extension().string();
    static const std::vector<std::string> binary_extensions = {
        ".bin", ".exe", ".dll", ".so", ".dylib", ".o", ".obj",
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico",
        ".zip", ".tar", ".gz", ".bz2", ".7z", ".rar",
        ".pdf", ".doc", ".xls", ".ppt"
    };
    for (const auto& bin_ext : binary_extensions) {
        if (ext == bin_ext) {
            return std::unexpected(EditError::BinaryFile);
        }
    }

    // 验证 old_text 不能为空
    if (op.old_text.empty()) {
        return std::unexpected(EditError::OldTextNotFound);
    }

    return {};
}

// 将 EditError 转换为可读描述
inline auto edit_error_to_string(EditError error) -> std::string_view {
    switch (error) {
        case EditError::FileNotFound:    return "File not found";
        case EditError::OldTextNotFound: return "Old text not found in file";
        case EditError::MultipleMatches: return "Multiple matches found; set replace_all=true or provide more context";
        case EditError::PermissionDenied: return "Permission denied";
        case EditError::BinaryFile:      return "Cannot edit binary file";
    }
    return "Unknown error";
}

} // namespace cc::tools
