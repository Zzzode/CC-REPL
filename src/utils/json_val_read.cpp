// Implementation unit for cc.utils.json — JsonVal read-only accessors
// (RFC 0001 Phase C batch 10). The textual <yyjson.h> lives here, not in the
// module interface.
module;

#include <cstdint>

#include <yyjson.h>

module cc.utils.json;

import std;

namespace cc::utils::json {

bool JsonVal::is_null() const noexcept { return yyjson_is_null(val_); }
bool JsonVal::is_bool() const noexcept { return yyjson_is_bool(val_); }
bool JsonVal::is_num() const noexcept { return yyjson_is_num(val_); }
bool JsonVal::is_int() const noexcept { return yyjson_is_int(val_); }
bool JsonVal::is_double() const noexcept { return yyjson_is_real(val_); }
bool JsonVal::is_str() const noexcept { return yyjson_is_str(val_); }
bool JsonVal::is_arr() const noexcept { return yyjson_is_arr(val_); }
bool JsonVal::is_obj() const noexcept { return yyjson_is_obj(val_); }

std::string_view JsonVal::as_str() const noexcept {
    const char* s = yyjson_get_str(val_);
    return s ? std::string_view(s, yyjson_get_len(val_)) : std::string_view{};
}
int64_t JsonVal::as_int() const noexcept { return yyjson_get_sint(val_); }
double JsonVal::as_double() const noexcept { return yyjson_get_num(val_); }
bool JsonVal::as_bool() const noexcept { return yyjson_get_bool(val_); }

JsonVal JsonVal::get(std::string_view key) const noexcept {
    return JsonVal(yyjson_obj_getn(val_, key.data(), key.size()));
}

JsonVal JsonVal::at(std::size_t index) const noexcept {
    return JsonVal(yyjson_arr_get(val_, index));
}

std::size_t JsonVal::size() const noexcept {
    if (is_arr()) return yyjson_arr_size(val_);
    if (is_obj()) return yyjson_obj_size(val_);
    return 0;
}

bool JsonVal::has(std::string_view key) const noexcept {
    return is_obj() && get(key).valid();
}

bool JsonVal::is_null(std::string_view key) const noexcept {
    auto child = get(key);
    return child.valid() && child.is_null();
}

std::string JsonVal::get_string(std::string_view key) const {
    auto child = get(key);
    return child.is_str() ? std::string(child.as_str()) : std::string{};
}

int64_t JsonVal::get_int(std::string_view key) const noexcept {
    auto child = get(key);
    return child.is_num() ? child.as_int() : 0;
}

std::optional<JsonVal> JsonVal::get_object(std::string_view key) const noexcept {
    auto child = get(key);
    if (!child.is_obj()) return std::nullopt;
    return child;
}

} // namespace cc::utils::json
