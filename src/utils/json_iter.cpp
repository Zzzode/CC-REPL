// Implementation unit for cc.utils.json — the four type-erased iterator
// walker bodies behind the inline iter/iter_obj template shells (RFC 0001
// Phase C batch 10). The textual <yyjson.h> lives here, not in the module
// interface.
module;

#include <cstddef>

#include <yyjson.h>

module cc.utils.json;

namespace cc::utils::json {

void JsonVal::iter_impl(IterCb cb, void* ctx) const {
    if (!is_arr()) return;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(val_, &iter);
    yyjson_val* item;
    while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
        cb(ctx, JsonVal(item));
    }
}

void JsonVal::iter_obj_impl(IterObjCb cb, void* ctx) const {
    if (!is_obj()) return;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(val_, &iter);
    yyjson_val* key;
    while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
        auto* value = yyjson_obj_iter_get_val(key);
        cb(ctx, JsonVal(key), JsonVal(value));
    }
}

void JsonMutVal::iter_impl(IterCb cb, void* ctx) const {
    if (!is_arr()) return;
    const size_t n = yyjson_mut_arr_size(val_);
    for (size_t i = 0; i < n; ++i) {
        auto* v = yyjson_mut_arr_get(val_, i);
        cb(ctx, JsonMutVal(v, doc_), i);
    }
}

void JsonMutVal::iter_obj_impl(IterObjCb cb, void* ctx) const {
    if (!is_obj()) return;
    yyjson_mut_obj_iter it;
    yyjson_mut_obj_iter_init(val_, &it);
    yyjson_mut_val* k;
    auto* doc_c = doc_;
    while ((k = yyjson_mut_obj_iter_next(&it)) != nullptr) {
        auto* v = yyjson_mut_obj_iter_get_val(k);
        cb(ctx, JsonMutVal(k, doc_c), JsonMutVal(v, doc_c));
    }
}

} // namespace cc::utils::json
