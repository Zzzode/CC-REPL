module;
#include <algorithm>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.treeify;

export namespace cc::utils {

namespace fs = std::filesystem;

struct TreeNode {
    std::string name;
    std::vector<TreeNode> children;
};

namespace detail {
    inline void render_tree_impl(const TreeNode& node, std::string& result,
                                  std::string_view prefix, bool is_last) {
        result += prefix;
        result += is_last ? "└── " : "├── ";
        result += node.name;
        result += "\n";

        std::string child_prefix(prefix);
        child_prefix += is_last ? "    " : "│   ";

        for (std::size_t i = 0; i < node.children.size(); ++i) {
            render_tree_impl(node.children[i], result, child_prefix, i == node.children.size() - 1);
        }
    }
} // namespace detail

// Render a tree structure with Unicode box-drawing characters
std::string render_tree(const TreeNode& root) {
    std::string result;
    result += root.name;
    result += "\n";

    for (std::size_t i = 0; i < root.children.size(); ++i) {
        detail::render_tree_impl(root.children[i], result, "", i == root.children.size() - 1);
    }

    return result;
}

// Convert a flat list of paths into a tree structure
TreeNode path_list_to_tree(std::span<fs::path> paths) {
    TreeNode root{".", {}};

    for (auto& p : paths) {
        TreeNode* current = &root;

        for (const auto& component : p) {
            std::string name = component.string();
            if (name == "." || name == "/") continue;

            // Find or create child
            bool found = false;
            for (auto& child : current->children) {
                if (child.name == name) {
                    current = &child;
                    found = true;
                    break;
                }
            }
            if (!found) {
                current->children.push_back(TreeNode{name, {}});
                current = &current->children.back();
            }
        }
    }

    return root;
}

// Render a directory tree from filesystem
std::string render_directory_tree(fs::path dir, int max_depth) {
    struct DirEntry {
        std::string name;
        bool is_dir;
        std::vector<DirEntry> children;
    };

    std::function<DirEntry(const fs::path&, int)> build_tree = [&](const fs::path& path, int depth) -> DirEntry {
        DirEntry entry;
        entry.name = path.filename().string();
        entry.is_dir = fs::is_directory(path);

        if (entry.is_dir && depth < max_depth) {
            std::error_code ec;
            for (auto& child : fs::directory_iterator(path, ec)) {
                if (ec) break;
                std::string child_name = child.path().filename().string();
                // Skip hidden files and common noise
                if (child_name.starts_with(".") || child_name == "node_modules" ||
                    child_name == "__pycache__" || child_name == "target") {
                    continue;
                }
                entry.children.push_back(build_tree(child.path(), depth + 1));
            }
            // Sort: directories first, then alphabetical
            std::sort(entry.children.begin(), entry.children.end(), [](const auto& a, const auto& b) {
                if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                return a.name < b.name;
            });
        }

        return entry;
    };

    auto tree = build_tree(dir, 0);

    // Convert to TreeNode and render
    std::function<TreeNode(const DirEntry&)> convert = [&](const DirEntry& entry) -> TreeNode {
        TreeNode node;
        node.name = entry.is_dir ? entry.name + "/" : entry.name;
        for (auto& child : entry.children) {
            node.children.push_back(convert(child));
        }
        return node;
    };

    return render_tree(convert(tree));
}

} // namespace cc::utils
