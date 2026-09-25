// Implementation unit for cc.utils.json — JsonMutDoc lifetime and factory
// methods (RFC 0001 Phase C batch 10). The textual <yyjson.h> lives here, not
// in the module interface.
module;

#include <cstdint>

#include <yyjson.h>

module cc.utils.json;

import std;

namespace cc::utils::json {

JsonMutDoc::JsonMutDoc() : doc_(yyjson_mut_doc_new(nullptr)) {}
JsonMutDoc::~JsonMutDoc() { if (doc_) yyjson_mut_doc_free(doc_); }

JsonMutDoc& JsonMutDoc::operator=(JsonMutDoc&& other) noexcept {
    if (this != &other) {
        if (doc_) yyjson_mut_doc_free(doc_);
        doc_ = std::exchange(other.doc_, nullptr);
    }
    return *this;
}

JsonMutVal JsonMutDoc::object() { return {yyjson_mut_obj(doc_), doc_}; }
JsonMutVal JsonMutDoc::array() { return {yyjson_mut_arr(doc_), doc_}; }
JsonMutVal JsonMutDoc::string(std::string_view s) {
    return {yyjson_mut_strncpy(doc_, s.data(), s.size()), doc_};
}
JsonMutVal JsonMutDoc::number(double n) { return {yyjson_mut_real(doc_, n), doc_}; }
JsonMutVal JsonMutDoc::number(int64_t n) { return {yyjson_mut_sint(doc_, n), doc_}; }
JsonMutVal JsonMutDoc::boolean(bool b) { return {yyjson_mut_bool(doc_, b), doc_}; }
JsonMutVal JsonMutDoc::null() { return {yyjson_mut_null(doc_), doc_}; }

JsonMutVal JsonMutDoc::copy_val(JsonVal val) {
    return {yyjson_val_mut_copy(doc_, val.raw()), doc_};
}

JsonMutVal JsonMutDoc::raw_json(std::string_view json_str) {
    auto* immutable_doc = yyjson_read(json_str.data(), json_str.size(), 0);
    if (!immutable_doc) return {nullptr, doc_};
    auto* root = yyjson_doc_get_root(immutable_doc);
    auto* copied = yyjson_val_mut_copy(doc_, root);
    yyjson_doc_free(immutable_doc);
    return {copied, doc_};
}

void JsonMutDoc::set_root(JsonMutVal val) { yyjson_mut_doc_set_root(doc_, val.raw()); }

JsonMutVal JsonMutDoc::root_mut() noexcept { return {yyjson_mut_doc_get_root(doc_), doc_}; }

yyjson_mut_doc* JsonMutDoc::raw() const noexcept { return doc_; }

} // namespace cc::utils::json
