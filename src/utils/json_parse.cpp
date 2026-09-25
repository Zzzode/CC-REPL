// Implementation unit for cc.utils.json — JsonDoc lifetime/delegates and the
// free parse* entry points (RFC 0001 Phase C batch 10). The textual
// <yyjson.h> lives here, not in the module interface.
module;

#include <cstdint>

#include <yyjson.h>

module cc.utils.json;

import std;

import cc.utils.error;

namespace cc::utils::json {

JsonDoc::~JsonDoc() { if (doc_) yyjson_doc_free(doc_); }

JsonDoc& JsonDoc::operator=(JsonDoc&& other) noexcept {
    if (this != &other) {
        if (doc_) yyjson_doc_free(doc_);
        doc_ = std::exchange(other.doc_, nullptr);
    }
    return *this;
}

JsonVal JsonDoc::root() const noexcept {
    return JsonVal(yyjson_doc_get_root(doc_));
}

bool JsonDoc::has(std::string_view key) const noexcept { return root().has(key); }
bool JsonDoc::is_null(std::string_view key) const noexcept { return root().is_null(key); }
std::string JsonDoc::get_string(std::string_view key) const { return root().get_string(key); }
int64_t JsonDoc::get_int(std::string_view key) const noexcept { return root().get_int(key); }
std::optional<JsonVal> JsonDoc::get_object(std::string_view key) const noexcept {
    return root().get_object(key);
}

Result<JsonDoc> parse(std::string_view json_str) {
    yyjson_read_err err;
    auto* doc = yyjson_read_opts(
        const_cast<char*>(json_str.data()), json_str.size(),
        0, nullptr, &err);

    if (!doc) {
        return std::unexpected(Error(ErrorCode::parse_error,
            std::format("JSON parse error at position {}: {}",
                err.pos, err.msg ? err.msg : "unknown")));
    }
    return JsonDoc(doc);
}

Result<JsonDoc> parse_first(std::string_view json_str) {
    yyjson_read_err err;
    auto* doc = yyjson_read_opts(
        const_cast<char*>(json_str.data()), json_str.size(),
        YYJSON_READ_STOP_WHEN_DONE, nullptr, &err);

    if (!doc) {
        return std::unexpected(Error(ErrorCode::parse_error,
            std::format("JSON parse error at position {}: {}",
                err.pos, err.msg ? err.msg : "unknown")));
    }
    return JsonDoc(doc);
}

Result<JsonDoc> parse_file_string(const std::string& path_string) {
    yyjson_read_err err;
    auto* doc = yyjson_read_file(
        path_string.c_str(), 0, nullptr, &err);

    if (!doc) {
        return std::unexpected(Error(ErrorCode::io_error,
            std::format("Failed to read JSON file '{}': {}",
                path_string, err.msg ? err.msg : "unknown")));
    }
    return JsonDoc(doc);
}

Result<JsonDoc> parse_file(std::string_view path) {
    return parse_file_string(std::string(path));
}

Result<JsonDoc> parse_file(const std::string& path) {
    return parse_file_string(path);
}

Result<JsonDoc> parse_file(const char* path) {
    return parse_file_string(std::string(path));
}

} // namespace cc::utils::json
