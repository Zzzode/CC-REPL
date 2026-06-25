module;

#include <cstddef>
#include <cstdlib>
#include <string>

#include <yyjson.h>

module cc.utils.json;

namespace cc::utils::json {

std::string JsonVal::to_string() const {
    if (!val_) return {};
    std::size_t len = 0;
    char* json = yyjson_val_write(val_, 0, &len);
    if (!json) return {};
    std::string result(json, len);
    free(json);
    return result;
}

std::string JsonMutDoc::to_string() const {
    std::size_t len = 0;
    char* json = yyjson_mut_write(doc_, 0, &len);
    if (!json) return {};
    std::string result(json, len);
    free(json);
    return result;
}

std::string JsonMutDoc::to_pretty_string() const {
    std::size_t len = 0;
    char* json = yyjson_mut_write(doc_, YYJSON_WRITE_PRETTY, &len);
    if (!json) return {};
    std::string result(json, len);
    free(json);
    return result;
}

std::string to_string(const JsonDoc& doc) {
    return doc.root().to_string();
}

std::string to_pretty_string(const JsonDoc& doc) {
    std::size_t len = 0;
    char* json = yyjson_val_write(doc.root().raw(), YYJSON_WRITE_PRETTY, &len);
    if (!json) return {};
    std::string result(json, len);
    free(json);
    return result;
}

} // namespace cc::utils::json
