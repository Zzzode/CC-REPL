// Implementation unit for cc.utils.json — JsonMutVal accessors and mutators
// (RFC 0001 Phase C batch 10). The textual <yyjson.h> lives here, not in the
// module interface.
module;

#include <cstddef>
#include <cstdint>

#include <yyjson.h>

module cc.utils.json;

import std;

namespace cc::utils::json {

bool JsonMutVal::is_null() const noexcept { return val_ && yyjson_mut_is_null(val_); }
bool JsonMutVal::is_bool() const noexcept { return val_ && yyjson_mut_is_bool(val_); }
bool JsonMutVal::is_num()  const noexcept { return val_ && yyjson_mut_is_num(val_); }
bool JsonMutVal::is_str()  const noexcept { return val_ && yyjson_mut_is_str(val_); }
bool JsonMutVal::is_arr()  const noexcept { return val_ && yyjson_mut_is_arr(val_); }
bool JsonMutVal::is_obj()  const noexcept { return val_ && yyjson_mut_is_obj(val_); }

std::string_view JsonMutVal::as_str() const noexcept {
    if (!is_str()) return {};
    const char* s = yyjson_mut_get_str(val_);
    if (!s) return {};
    size_t len = yyjson_mut_get_len(val_);
    return std::string_view(s, len);
}
int64_t JsonMutVal::as_int()   const noexcept { return is_num() ? yyjson_mut_get_sint(val_) : 0; }
double  JsonMutVal::as_double() const noexcept { return is_num() ? yyjson_mut_get_num(val_) : 0.0; }
bool    JsonMutVal::as_bool() const noexcept { return is_bool() && yyjson_mut_get_bool(val_); }
std::size_t JsonMutVal::size() const noexcept {
    if (is_arr()) return yyjson_mut_arr_size(val_);
    if (is_obj()) return yyjson_mut_obj_size(val_);
    return 0;
}

void JsonMutVal::add(std::string_view key, JsonMutVal value) {
    if (!val_ || !doc_) return;
    auto* k = yyjson_mut_strncpy(doc_, key.data(), key.size());
    yyjson_mut_obj_put(val_, k, value.val_);
}
void JsonMutVal::append(JsonMutVal value) {
    if (is_arr()) yyjson_mut_arr_append(val_, value.val_);
}
bool JsonMutVal::remove(std::string_view key) {
    if (!is_obj()) return false;
    return yyjson_mut_obj_remove_keyn(val_, key.data(), key.size()) != nullptr;
}
bool JsonMutVal::has(std::string_view key) const noexcept {
    if (!is_obj()) return false;
    return yyjson_mut_obj_getn(val_, key.data(), key.size()) != nullptr;
}
JsonMutVal JsonMutVal::get(std::string_view key) noexcept {
    if (!is_obj()) return {};
    auto* child = yyjson_mut_obj_getn(val_, key.data(), key.size());
    return {child, doc_};
}
JsonMutVal JsonMutVal::at(std::size_t idx) noexcept {
    if (!is_arr()) return {};
    auto* child = yyjson_mut_arr_get(val_, idx);
    return {child, doc_};
}

void JsonMutVal::set(std::string_view k, std::string_view v) { add(k, make_str(v)); }
void JsonMutVal::set(std::string_view k, const char* v)    { add(k, make_str(v ? std::string_view(v) : std::string_view{})); }
void JsonMutVal::set(std::string_view k, int64_t v)       { add(k, make_num(v)); }
void JsonMutVal::set(std::string_view k, int v)              { add(k, make_num(static_cast<int64_t>(v))); }
void JsonMutVal::set(std::string_view k, double v)            { add(k, make_real(v)); }
void JsonMutVal::set(std::string_view k, bool v)              { add(k, make_bool(v)); }

JsonMutVal JsonMutVal::make_obj()   { return {yyjson_mut_obj(doc_), doc_}; }
JsonMutVal JsonMutVal::make_arr() { return {yyjson_mut_arr(doc_), doc_}; }
JsonMutVal JsonMutVal::make_str(std::string_view s) {
    return {yyjson_mut_strncpy(doc_, s.data(), s.size()), doc_};
}
JsonMutVal JsonMutVal::make_num(int64_t n) { return {yyjson_mut_sint(doc_, n), doc_}; }
JsonMutVal JsonMutVal::make_real(double r) { return {yyjson_mut_real(doc_, r), doc_}; }
JsonMutVal JsonMutVal::make_bool(bool b) { return {yyjson_mut_bool(doc_, b), doc_}; }
JsonMutVal JsonMutVal::make_null() { return {yyjson_mut_null(doc_), doc_}; }

JsonMutVal JsonMutVal::ensure_object(std::string_view key) {
    auto existing = get(key);
    if (existing.valid() && existing.is_obj()) return existing;
    auto child = make_obj();
    add(key, child);
    return child;
}
JsonMutVal JsonMutVal::ensure_array(std::string_view key) {
    auto existing = get(key);
    if (existing.valid() && existing.is_arr()) return existing;
    auto child = make_arr();
    add(key, child);
    return child;
}

} // namespace cc::utils::json
