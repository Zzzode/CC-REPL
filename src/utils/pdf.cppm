// C++23 Module: PDF processing

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


struct PdfPage {
    size_t page_number;
    std::string text_content;
};


[[nodiscard]] inline bool is_pdf(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    std::array<char, 5> header{};
    file.read(header.data(), 5);
    if (file.gcount() < 5) return false;

    return std::string_view(header.data(), 5) == "%PDF-";
}


[[nodiscard]] inline std::expected<size_t, std::string> get_page_count(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("Cannot open file: {}", path.string()));
    }


    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());


    size_t count = 0;
    size_t pos = 0;
    while ((pos = content.find("/Type /Page", pos)) != std::string::npos) {

        size_t after = pos + 11;
        if (after < content.size() && content[after] == 's') {
            pos = after;
            continue;
        }
        ++count;
        pos = after;
    }

    return count > 0 ? count : 1;
}


[[nodiscard]] inline std::string extract_text_stream(std::string_view content) {
    std::string result;
    size_t pos = 0;

    while (pos < content.size()) {

        auto bt = content.find("BT", pos);
        if (bt == std::string_view::npos) break;

        auto et = content.find("ET", bt);
        if (et == std::string_view::npos) break;

        auto block = content.substr(bt + 2, et - bt - 2);


        size_t bi = 0;
        while (bi < block.size()) {

            if (block[bi] == '(') {
                ++bi;
                std::string text;
                int depth = 1;
                while (bi < block.size() && depth > 0) {
                    if (block[bi] == '\\' && bi + 1 < block.size()) {

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

        pages.push_back({1, "[No extractable text - possibly a scanned document]"});
        return pages;
    }


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
