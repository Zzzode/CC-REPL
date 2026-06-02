// C++23 Module: PDF processing
// PDF 文本提取与转换 (基于 magic bytes 检测和基础文本流解析)
module;
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.pdf;


export namespace cc::utils::pdf {

// PDF 页面内容
struct PdfPage {
    size_t page_number;
    std::string text_content;
};

// 检查文件是否为 PDF (magic byte: %PDF-)
[[nodiscard]] inline bool is_pdf(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    std::array<char, 5> header{};
    file.read(header.data(), 5);
    if (file.gcount() < 5) return false;

    return std::string_view(header.data(), 5) == "%PDF-";
}

// 获取 PDF 页数 (基于 /Type /Page 计数)
[[nodiscard]] inline std::expected<size_t, std::string> get_page_count(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("Cannot open file: {}", path.string()));
    }

    // 读取全部内容
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // 统计 /Type /Page 出现次数 (排除 /Type /Pages)
    size_t count = 0;
    size_t pos = 0;
    while ((pos = content.find("/Type /Page", pos)) != std::string::npos) {
        // 确保不是 /Type /Pages
        size_t after = pos + 11;
        if (after < content.size() && content[after] == 's') {
            pos = after;
            continue;
        }
        ++count;
        pos = after;
    }

    return count > 0 ? count : 1;  // 至少1页
}

// 从 PDF 二进制流中提取文本段
[[nodiscard]] inline std::string extract_text_stream(std::string_view content) {
    std::string result;
    size_t pos = 0;

    while (pos < content.size()) {
        // 查找文本对象 BT...ET
        auto bt = content.find("BT", pos);
        if (bt == std::string_view::npos) break;

        auto et = content.find("ET", bt);
        if (et == std::string_view::npos) break;

        auto block = content.substr(bt + 2, et - bt - 2);

        // 提取 Tj 和 TJ 操作符中的文本
        size_t bi = 0;
        while (bi < block.size()) {
            // 查找括号中的文本字符串 (...)
            if (block[bi] == '(') {
                ++bi;
                std::string text;
                int depth = 1;
                while (bi < block.size() && depth > 0) {
                    if (block[bi] == '\\' && bi + 1 < block.size()) {
                        // 处理转义字符
                        ++bi;
                        switch (block[bi]) {
                            case 'n': text += '\n'; break;
                            case 'r': text += '\r'; break;
                            case 't': text += '\t'; break;
                            case '(': text += '('; break;
                            case ')': text += ')'; break;
                            case '\\': text += '\\'; break;
                            default: text += block[bi]; break;
                        }
                    } else if (block[bi] == '(') {
                        ++depth;
                        text += '(';
                    } else if (block[bi] == ')') {
                        --depth;
                        if (depth > 0) text += ')';
                    } else {
                        text += block[bi];
                    }
                    ++bi;
                }
                if (!text.empty()) result += text;
            } else if (block[bi] == 'T' && bi + 1 < block.size()) {
                // Td/TD 操作表示行分隔
                if (block[bi + 1] == 'd' || block[bi + 1] == 'D') {
                    result += '\n';
                }
                bi += 2;
            } else {
                ++bi;
            }
        }

        pos = et + 2;
    }

    return result;
}

// 提取 PDF 文本 (按页)
[[nodiscard]] inline std::expected<std::vector<PdfPage>, std::string> extract_text(
    const std::filesystem::path& pdf_path) {
    if (!is_pdf(pdf_path)) {
        return std::unexpected(std::format("Not a valid PDF file: {}", pdf_path.string()));
    }

    std::ifstream file(pdf_path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("Cannot open file: {}", pdf_path.string()));
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    std::vector<PdfPage> pages;
    std::string full_text = extract_text_stream(content);

    if (full_text.empty()) {
        // 无法提取文本 (可能是扫描件)
        pages.push_back({1, "[No extractable text - possibly a scanned document]"});
        return pages;
    }

    // 按换页符或大段空白分页
    size_t page_num = 1;
    size_t pos = 0;
    while (pos < full_text.size()) {
        auto next_page = full_text.find('\f', pos);
        std::string page_text;
        if (next_page != std::string::npos) {
            page_text = full_text.substr(pos, next_page - pos);
            pos = next_page + 1;
        } else {
            page_text = full_text.substr(pos);
            pos = full_text.size();
        }
        // 去除首尾空白
        if (auto start = page_text.find_first_not_of(" \t\n\r");
            start != std::string::npos) {
            auto end = page_text.find_last_not_of(" \t\n\r");
            page_text = page_text.substr(start, end - start + 1);
        }
        if (!page_text.empty()) {
            pages.push_back({page_num, std::move(page_text)});
        }
        ++page_num;
    }

    if (pages.empty()) {
        pages.push_back({1, full_text});
    }

    return pages;
}

// 将 PDF 转换为 Markdown 格式
[[nodiscard]] inline std::expected<std::string, std::string> pdf_to_markdown(
    const std::filesystem::path& pdf_path) {
    auto pages_result = extract_text(pdf_path);
    if (!pages_result) {
        return std::unexpected(pages_result.error());
    }

    const auto& pages = *pages_result;
    std::string markdown;
    markdown.reserve(pages.size() * 1024);

    markdown += std::format("# {}\n\n", pdf_path.stem().string());

    for (const auto& page : pages) {
        if (pages.size() > 1) {
            markdown += std::format("## Page {}\n\n", page.page_number);
        }
        markdown += page.text_content;
        markdown += "\n\n";
    }

    return markdown;
}

} // namespace cc::utils::pdf
