// C++23 JSON RAII Wrapper Module (based on yyjson)
// Provides safe, ergonomic C++ interface for high-performance JSON parsing/building
module;

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yyjson.h>

export module cc.utils.json;

import cc.utils.error;

export namespace cc::utils::json {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;


class JsonVal;
class JsonMutVal;
class JsonMutDoc;

// =========================================================================

// =========================================================================
class JsonVal {
public:
    explicit JsonVal(yyjson_val* val = nullptr) noexcept : val_(val) {}

    [[nodiscard]] bool valid() const noexcept { return val_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }


    [[nodiscard]] bool is_null() const noexcept { return yyjson_is_null(val_); }
    [[nodiscard]] bool is_bool() const noexcept { return yyjson_is_bool(val_); }
    [[nodiscard]] bool is_num() const noexcept { return yyjson_is_num(val_); }
    // yyjson distinguishes integer from real (floating-point) numbers; these
    // discriminators let callers branch on the underlying numeric subtype.
    [[nodiscard]] bool is_int() const noexcept { return yyjson_is_int(val_); }
    [[nodiscard]] bool is_double() const noexcept { return yyjson_is_real(val_); }
    [[nodiscard]] bool is_str() const noexcept { return yyjson_is_str(val_); }
    [[nodiscard]] bool is_arr() const noexcept { return yyjson_is_arr(val_); }
    [[nodiscard]] bool is_obj() const noexcept { return yyjson_is_obj(val_); }


    [[nodiscard]] std::string_view as_str() const noexcept {
        const char* s = yyjson_get_str(val_);
        return s ? std::string_view(s, yyjson_get_len(val_)) : std::string_view{};
    }
    [[nodiscard]] int64_t as_int() const noexcept { return yyjson_get_sint(val_); }
    [[nodiscard]] double as_double() const noexcept { return yyjson_get_num(val_); }
    [[nodiscard]] bool as_bool() const noexcept { return yyjson_get_bool(val_); }


    [[nodiscard]] JsonVal get(std::string_view key) const noexcept {
        return JsonVal(yyjson_obj_getn(val_, key.data(), key.size()));
    }


    [[nodiscard]] JsonVal at(std::size_t index) const noexcept {
        return JsonVal(yyjson_arr_get(val_, index));
    }


    [[nodiscard]] std::size_t size() const noexcept {
        if (is_arr()) return yyjson_arr_size(val_);
        if (is_obj()) return yyjson_obj_size(val_);
        return 0;
    }


    template<typename Fn>
    void iter(Fn&& fn) const {
        if (!is_arr()) return;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(val_, &iter);
        yyjson_val* item;
        while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
            fn(JsonVal(item));
        }
    }


    template<typename Fn>
    void iter_obj(Fn&& fn) const {
        if (!is_obj()) return;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(val_, &iter);
        yyjson_val* key;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
            auto* value = yyjson_obj_iter_get_val(key);
            fn(JsonVal(key), JsonVal(value));
        }
    }

    [[nodiscard]] yyjson_val* raw() const noexcept { return val_; }

    [[nodiscard]] bool has(std::string_view key) const noexcept {
        return is_obj() && get(key).valid();
    }

    [[nodiscard]] bool is_null(std::string_view key) const noexcept {
        auto child = get(key);
        return child.valid() && child.is_null();
    }

    [[nodiscard]] std::string get_string(std::string_view key) const {
        auto child = get(key);
        return child.is_str() ? std::string(child.as_str()) : std::string{};
    }

    [[nodiscard]] int64_t get_int(std::string_view key) const noexcept {
        auto child = get(key);
        return child.is_num() ? child.as_int() : 0;
    }

    [[nodiscard]] std::optional<JsonVal> get_object(std::string_view key) const noexcept {
        auto child = get(key);
        if (!child.is_obj()) return std::nullopt;
        return child;
    }

    [[nodiscard]] std::string to_string() const {
        if (!val_) return {};
        std::size_t len = 0;
        char* json = yyjson_val_write(val_, 0, &len);
        if (!json) return {};
        std::string result(json, len);
        free(json);
        return result;
    }

private:
    yyjson_val* val_;
};

// =========================================================================

// =========================================================================
class JsonDoc {
public:
    JsonDoc() noexcept : doc_(nullptr) {}
    explicit JsonDoc(yyjson_doc* doc) noexcept : doc_(doc) {}

    ~JsonDoc() { if (doc_) yyjson_doc_free(doc_); }


    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;
    JsonDoc(JsonDoc&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}
    JsonDoc& operator=(JsonDoc&& other) noexcept {
        if (this != &other) {
            if (doc_) yyjson_doc_free(doc_);
            doc_ = std::exchange(other.doc_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return doc_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }


    [[nodiscard]] JsonVal root() const noexcept {
        return JsonVal(yyjson_doc_get_root(doc_));
    }

    [[nodiscard]] yyjson_doc* raw() const noexcept { return doc_; }

    [[nodiscard]] bool has(std::string_view key) const noexcept { return root().has(key); }
    [[nodiscard]] bool is_null(std::string_view key) const noexcept { return root().is_null(key); }
    [[nodiscard]] std::string get_string(std::string_view key) const { return root().get_string(key); }
    [[nodiscard]] int64_t get_int(std::string_view key) const noexcept { return root().get_int(key); }
    [[nodiscard]] std::optional<JsonVal> get_object(std::string_view key) const noexcept {
        return root().get_object(key);
    }

private:
    yyjson_doc* doc_;
};

// =========================================================================

// =========================================================================
class JsonMutVal {
public:
    JsonMutVal() noexcept : val_(nullptr), doc_(nullptr) {}
    JsonMutVal(yyjson_mut_val* val, yyjson_mut_doc* doc) noexcept
        : val_(val), doc_(doc) {}

    [[nodiscard]] bool valid() const noexcept { return val_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] bool is_null() const noexcept { return val_ && yyjson_mut_is_null(val_); }
    [[nodiscard]] bool is_bool() const noexcept { return val_ && yyjson_mut_is_bool(val_); }
    [[nodiscard]] bool is_num()  const noexcept { return val_ && yyjson_mut_is_num(val_); }
    [[nodiscard]] bool is_str()  const noexcept { return val_ && yyjson_mut_is_str(val_); }
    [[nodiscard]] bool is_arr()  const noexcept { return val_ && yyjson_mut_is_arr(val_); }
    [[nodiscard]] bool is_obj()  const noexcept { return val_ && yyjson_mut_is_obj(val_); }

    /// Read-typed accessors (read the *mut* value as a primitive).
    [[nodiscard]] std::string_view as_str() const noexcept {
        if (!is_str()) return {};
        const char* s = yyjson_mut_get_str(val_);
        if (!s) return {};
        size_t len = yyjson_mut_get_len(val_);
        return std::string_view(s, len);
    }
    [[nodiscard]] int64_t as_int()   const noexcept { return is_num() ? yyjson_mut_get_sint(val_) : 0; }
    [[nodiscard]] double  as_double() const noexcept { return is_num() ? yyjson_mut_get_num(val_) : 0.0; }
    [[nodiscard]] bool    as_bool() const noexcept { return is_bool() && yyjson_mut_get_bool(val_); }
    [[nodiscard]] std::size_t size() const noexcept {
        if (is_arr()) return yyjson_mut_arr_size(val_);
        if (is_obj()) return yyjson_mut_obj_size(val_);
        return 0;
    }

    /// Add / replace a key on an object.  If the key already exists it is replaced
    /// with the new value (yyjson_mut_obj_put semantics).
    void add(std::string_view key, JsonMutVal value) {
        if (!val_ || !doc_) return;
        auto* k = yyjson_mut_strncpy(doc_, key.data(), key.size());
        yyjson_mut_obj_put(val_, k, value.val_);
    }
    void append(JsonMutVal value) {
        if (is_arr()) yyjson_mut_arr_append(val_, value.val_);
    }
    /// Remove a key from an object.  Returns true if the key existed.
    [[nodiscard]] bool remove(std::string_view key) {
        if (!is_obj()) return false;
        return yyjson_mut_obj_remove_keyn(val_, key.data(), key.size()) != nullptr;
    }
    /// Check whether an object contains the given key.
    [[nodiscard]] bool has(std::string_view key) const noexcept {
        if (!is_obj()) return false;
        return yyjson_mut_obj_getn(val_, key.data(), key.size()) != nullptr;
    }
    /// Retrieve a child by key (mutable view into the same document).
    [[nodiscard]] JsonMutVal get(std::string_view key) noexcept {
        if (!is_obj()) return {};
        auto* child = yyjson_mut_obj_getn(val_, key.data(), key.size());
        return {child, doc_};
    }
    /// Retrieve a child by index (mutable, for arrays).
    [[nodiscard]] JsonMutVal at(std::size_t idx) noexcept {
        if (!is_arr()) return {};
        auto* child = yyjson_mut_arr_get(val_, idx);
        return {child, doc_};
    }

    // Convenience typed setters.  These call add() with the appropriate builder.
    void set(std::string_view k, std::string_view v) { add(k, make_str(v)); }
    void set(std::string_view k, const char* v)    { add(k, make_str(v ? std::string_view(v) : std::string_view{})); }
    void set(std::string_view k, int64_t v)       { add(k, make_num(v)); }
    void set(std::string_view k, int v)              { add(k, make_num(static_cast<int64_t>(v))); }
    void set(std::string_view k, double v)            { add(k, make_real(v)); }
    void set(std::string_view k, bool v)              { add(k, make_bool(v)); }

    /// Return a newly-constructed mutable value owned by the underlying document.
    [[nodiscard]] JsonMutVal make_obj()   { return {yyjson_mut_obj(doc_), doc_}; }
    [[nodiscard]] JsonMutVal make_arr() { return {yyjson_mut_arr(doc_), doc_}; }
    [[nodiscard]] JsonMutVal make_str(std::string_view s) {
        return {yyjson_mut_strncpy(doc_, s.data(), s.size()), doc_};
    }
    [[nodiscard]] JsonMutVal make_num(int64_t n) { return {yyjson_mut_sint(doc_, n), doc_}; }
    [[nodiscard]] JsonMutVal make_real(double r) { return {yyjson_mut_real(doc_, r), doc_}; }
    [[nodiscard]] JsonMutVal make_bool(bool b) { return {yyjson_mut_bool(doc_, b), doc_}; }
    [[nodiscard]] JsonMutVal make_null() { return {yyjson_mut_null(doc_), doc_}; }

    /// Ensure that a child object with the given name exists, creating it if
    /// necessary.  Returns a mutable view of the child.
    [[nodiscard]] JsonMutVal ensure_object(std::string_view key) {
        auto existing = get(key);
        if (existing.valid() && existing.is_obj()) return existing;
        auto child = make_obj();
        add(key, child);
        return child;
    }
    /// Ensure that a child array with the given name exists, creating it if necessary.
    [[nodiscard]] JsonMutVal ensure_array(std::string_view key) {
        auto existing = get(key);
        if (existing.valid() && existing.is_arr()) return existing;
        auto child = make_arr();
        add(key, child);
        return child;
    }

    // Iteration helpers — allow safe traversal without touching yyjson C API
    // symbols directly (important for C++ module BMI hygiene).
    template<typename Fn>
    void iter_obj(Fn&& fn) const {
        if (!is_obj()) return;
        yyjson_mut_obj_iter it;
        yyjson_mut_obj_iter_init(val_, &it);
        yyjson_mut_val* k;
        auto* doc_c = doc_;
        while ((k = yyjson_mut_obj_iter_next(&it)) != nullptr) {
            auto* v = yyjson_mut_obj_iter_get_val(k);
            fn(JsonMutVal(k, doc_c), JsonMutVal(v, doc_c));
        }
    }

    template<typename Fn>
    void iter(Fn&& fn) const {
        if (!is_arr()) return;
        const size_t n = yyjson_mut_arr_size(val_);
        for (size_t i = 0; i < n; ++i) {
            auto* v = yyjson_mut_arr_get(val_, i);
            fn(JsonMutVal(v, doc_), i);
        }
    }

    [[nodiscard]] yyjson_mut_val* raw() const noexcept { return val_; }
    [[nodiscard]] yyjson_mut_doc* doc() const noexcept { return doc_; }

private:
    yyjson_mut_val* val_;
    yyjson_mut_doc* doc_;
};

// =========================================================================

// =========================================================================
class JsonMutDoc {
public:
    JsonMutDoc() : doc_(yyjson_mut_doc_new(nullptr)) {}
    ~JsonMutDoc() { if (doc_) yyjson_mut_doc_free(doc_); }

    JsonMutDoc(const JsonMutDoc&) = delete;
    JsonMutDoc& operator=(const JsonMutDoc&) = delete;
    JsonMutDoc(JsonMutDoc&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}
    JsonMutDoc& operator=(JsonMutDoc&& other) noexcept {
        if (this != &other) {
            if (doc_) yyjson_mut_doc_free(doc_);
            doc_ = std::exchange(other.doc_, nullptr);
        }
        return *this;
    }


    [[nodiscard]] JsonMutVal object() { return {yyjson_mut_obj(doc_), doc_}; }
    [[nodiscard]] JsonMutVal array() { return {yyjson_mut_arr(doc_), doc_}; }
    [[nodiscard]] JsonMutVal string(std::string_view s) {
        return {yyjson_mut_strncpy(doc_, s.data(), s.size()), doc_};
    }
    [[nodiscard]] JsonMutVal number(double n) { return {yyjson_mut_real(doc_, n), doc_}; }
    [[nodiscard]] JsonMutVal number(int64_t n) { return {yyjson_mut_sint(doc_, n), doc_}; }
    [[nodiscard]] JsonMutVal boolean(bool b) { return {yyjson_mut_bool(doc_, b), doc_}; }
    [[nodiscard]] JsonMutVal null() { return {yyjson_mut_null(doc_), doc_}; }

    // Copy an immutable JsonVal into this mutable document
    [[nodiscard]] JsonMutVal copy_val(JsonVal val) {
        return {yyjson_val_mut_copy(doc_, val.raw()), doc_};
    }

    // Parse a raw JSON string and embed it as a mutable value in this document
    [[nodiscard]] JsonMutVal raw_json(std::string_view json_str) {
        auto* immutable_doc = yyjson_read(json_str.data(), json_str.size(), 0);
        if (!immutable_doc) return {nullptr, doc_};
        auto* root = yyjson_doc_get_root(immutable_doc);
        auto* copied = yyjson_val_mut_copy(doc_, root);
        yyjson_doc_free(immutable_doc);
        return {copied, doc_};
    }


    void set_root(JsonMutVal val) { yyjson_mut_doc_set_root(doc_, val.raw()); }

    /// Mutable-root accessor.  Returns a mutable view of the document's root
    /// value (or an invalid JsonMutVal when no root is set), enabling in-place
    /// reads/edits of an already-committed tree without re-parsing.
    [[nodiscard]] JsonMutVal root_mut() noexcept { return {yyjson_mut_doc_get_root(doc_), doc_}; }


    [[nodiscard]] std::string to_string() const {
        std::size_t len = 0;
        char* json = yyjson_mut_write(doc_, 0, &len);
        if (!json) return {};
        std::string result(json, len);
        free(json);
        return result;
    }


    [[nodiscard]] std::string to_pretty_string() const {
        std::size_t len = 0;
        char* json = yyjson_mut_write(doc_, YYJSON_WRITE_PRETTY, &len);
        if (!json) return {};
        std::string result(json, len);
        free(json);
        return result;
    }

    [[nodiscard]] yyjson_mut_doc* raw() const noexcept { return doc_; }

private:
    yyjson_mut_doc* doc_;
};

class JsonObject {
public:
    JsonObject() : root_(doc_.object()) { doc_.set_root(root_); }

    void set(std::string_view key, std::string_view value) { root_.add(key, doc_.string(value)); }
    void set(std::string_view key, const char* value) { root_.add(key, doc_.string(value ? std::string_view(value) : std::string_view{})); }
    void set(std::string_view key, int64_t value) { root_.add(key, doc_.number(value)); }
    void set(std::string_view key, int value) { root_.add(key, doc_.number(static_cast<int64_t>(value))); }
    void set(std::string_view key, double value) { root_.add(key, doc_.number(value)); }
    void set(std::string_view key, bool value) { root_.add(key, doc_.boolean(value)); }

    [[nodiscard]] std::string serialize() const { return doc_.to_string(); }

private:
    JsonMutDoc doc_;
    JsonMutVal root_;
};

class JsonArray {
public:
    JsonArray() : root_(doc_.array()) { doc_.set_root(root_); }

    void push(int64_t value) { root_.append(doc_.number(value)); ints_.push_back(value); ++size_; }
    void push(int value) { push(static_cast<int64_t>(value)); }
    void push(std::string_view value) { root_.append(doc_.string(value)); ++size_; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] int64_t get_int(std::size_t index) const {
        return index < ints_.size() ? ints_[index] : 0;
    }
    [[nodiscard]] std::string serialize() const { return doc_.to_string(); }

private:
    JsonMutDoc doc_;
    JsonMutVal root_;
    std::vector<int64_t> ints_;
    std::size_t size_ = 0;
};

[[nodiscard]] inline JsonObject object() { return JsonObject{}; }
[[nodiscard]] inline JsonArray array() { return JsonArray{}; }

// =========================================================================

// =========================================================================


/// Fluent builder over JsonMutDoc for declarative object construction.
/// Provides chainable str/boolean/num setters; doc()/root() expose the
/// underlying mutable document for nested array/object assembly, and
/// serialize() commits the root and returns the JSON text.
class JsonBuilder {
public:
    JsonBuilder() : root_(doc_.object()) {}

    JsonBuilder& str(std::string_view key, std::string_view value) {
        root_.add(key, doc_.string(value));
        return *this;
    }
    JsonBuilder& boolean(std::string_view key, bool value) {
        root_.add(key, doc_.boolean(value));
        return *this;
    }
    JsonBuilder& num(std::string_view key, int64_t value) {
        root_.add(key, doc_.number(value));
        return *this;
    }
    JsonBuilder& num(std::string_view key, double value) {
        root_.add(key, doc_.number(value));
        return *this;
    }
    /// Adds the field only when the optional holds a value.
    JsonBuilder& opt_str(std::string_view key, const std::optional<std::string>& value) {
        if (value) root_.add(key, doc_.string(*value));
        return *this;
    }
    /// Adds a numeric count field (alias of num for collection sizes / counters).
    JsonBuilder& size(std::string_view key, std::size_t value) {
        root_.add(key, doc_.number(static_cast<int64_t>(value)));
        return *this;
    }

    JsonMutDoc& doc() noexcept { return doc_; }
    JsonMutVal root() noexcept { return root_; }

    std::string serialize() {
        doc_.set_root(root_);
        return doc_.to_string();
    }

private:
    JsonMutDoc doc_;
    JsonMutVal root_;
};

[[nodiscard]] inline Result<JsonDoc> parse(std::string_view json_str) {
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


[[nodiscard]] inline Result<JsonDoc> parse_file_string(const std::string& path_string) {
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

[[nodiscard]] inline Result<JsonDoc> parse_file(std::string_view path) {
    return parse_file_string(std::string(path));
}

[[nodiscard]] inline Result<JsonDoc> parse_file(const std::string& path) {
    return parse_file_string(path);
}

[[nodiscard]] inline Result<JsonDoc> parse_file(const char* path) {
    return parse_file_string(std::string(path));
}

template <typename Path>
[[nodiscard]] inline Result<JsonDoc> parse_file(const Path& path) {
    return parse_file_string(path.string());
}


[[nodiscard]] inline std::string to_string(const JsonDoc& doc) {
    std::size_t len = 0;
    char* json = yyjson_write(doc.raw(), 0, &len);
    if (!json) return {};
    std::string result(json, len);
    free(json);
    return result;
}

[[nodiscard]] inline std::string to_pretty_string(const JsonDoc& doc) {
    std::size_t len = 0;
    char* json = yyjson_write(doc.raw(), YYJSON_WRITE_PRETTY, &len);
    if (!json) return {};
    std::string result(json, len);
    free(json);
    return result;
}

[[nodiscard]] inline std::string to_string(JsonVal val) {
    return val.to_string();
}

} // namespace cc::utils::json
