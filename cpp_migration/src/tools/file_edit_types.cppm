module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <expected>
#include <optional>

export module cc.tools.file_edit_types;

export namespace cc::tools {


struct EditOperation {
    std::filesystem::path file;
    std::string old_text;
    std::string new_text;
    bool replace_all = false;
};


struct EditResult {
    bool success;
    int replacements;
    std::vector<int> affected_lines;
    std::string preview;
};


enum class EditError {
    FileNotFound,
    OldTextNotFound,
    MultipleMatches,
    PermissionDenied,
    BinaryFile
};


inline auto validate_edit(const EditOperation& op) -> std::expected<void, EditError> {
    namespace fs = std::filesystem;


    if (!fs::exists(op.file)) {
        return std::unexpected(EditError::FileNotFound);
    }


    auto perms = fs::status(op.file).permissions();
    if ((perms & fs::perms::owner_write) == fs::perms::none) {
        return std::unexpected(EditError::PermissionDenied);
    }



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


    if (op.old_text.empty()) {
        return std::unexpected(EditError::OldTextNotFound);
    }

    return {};
}


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
