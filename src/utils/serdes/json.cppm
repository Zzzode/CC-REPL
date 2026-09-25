// C++23 JSON RAII Wrapper Module (based on yyjson)
// Provides safe, ergonomic C++ interface for high-performance JSON parsing/building
//
// RFC 0001 Phase C batch 10 split: the textual <yyjson.h> header is confined
// to the module implementation units (utils/serdes/json_impl.cpp, json_val_read.cpp,
// json_parse.cpp, json_mut_val.cpp, json_mut_doc.cpp, json_iter.cpp,
// json_builders.cpp). This interface unit sees only the four opaque yyjson
// pointer types below — no sizeof, dereference, field access or yyjson call
// may appear here. Member bodies that touch yyjson live out-of-line.
module;

#include <cstddef>
#include <cstdint>

// Opaque yyjson types — every use in this interface is pointer-only. The
// canonical definitions (typedef struct yyjson_val yyjson_val; and friends)
// come from <yyjson.h>, textually included by each implementation unit and
// by external TUs that need the concrete types (e.g. concrete_migrations.cpp).
struct yyjson_val;
struct yyjson_doc;
struct yyjson_mut_val;
struct yyjson_mut_doc;

export module cc.utils.json;

import std;

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


    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_num() const noexcept;
    // yyjson distinguishes integer from real (floating-point) numbers; these
    // discriminators let callers branch on the underlying numeric subtype.
    [[nodiscard]] bool is_int() const noexcept;
    [[nodiscard]] bool is_double() const noexcept;
    [[nodiscard]] bool is_str() const noexcept;
    [[nodiscard]] bool is_arr() const noexcept;
    [[nodiscard]] bool is_obj() const noexcept;


    [[nodiscard]] std::string_view as_str() const noexcept;
    [[nodiscard]] int64_t as_int() const noexcept;
    [[nodiscard]] double as_double() const noexcept;
    [[nodiscard]] bool as_bool() const noexcept;


    [[nodiscard]] JsonVal get(std::string_view key) const noexcept;


    [[nodiscard]] JsonVal at(std::size_t index) const noexcept;


    [[nodiscard]] std::size_t size() const noexcept;


    // Type-erased iteration shells: the trampoline is a non-capturing static
    // function (convertible to the IterCb function pointer) and the caller's
    // callable — capturing lambdas included — rides through as an opaque
    // context pointer. The yyjson walk itself lives in json_iter.cpp.
    template<class Fn>
    void iter(Fn&& fn) const {
        iter_impl([](void* c, JsonVal item) {
            (*static_cast<std::remove_reference_t<Fn>*>(c))(item);
        }, static_cast<void*>(std::addressof(fn)));
    }


    template<class Fn>
    void iter_obj(Fn&& fn) const {
        iter_obj_impl([](void* c, JsonVal key, JsonVal value) {
            (*static_cast<std::remove_reference_t<Fn>*>(c))(key, value);
        }, static_cast<void*>(std::addressof(fn)));
    }

    [[nodiscard]] yyjson_val* raw() const noexcept { return val_; }

    [[nodiscard]] bool has(std::string_view key) const noexcept;

    [[nodiscard]] bool is_null(std::string_view key) const noexcept;

    [[nodiscard]] std::string get_string(std::string_view key) const;

    [[nodiscard]] int64_t get_int(std::string_view key) const noexcept;

    [[nodiscard]] std::optional<JsonVal> get_object(std::string_view key) const noexcept;

    [[nodiscard]] std::string to_string() const;

private:
    using IterCb = void(*)(void* ctx, JsonVal item);
    using IterObjCb = void(*)(void* ctx, JsonVal key, JsonVal value);

    void iter_impl(IterCb cb, void* ctx) const;
    void iter_obj_impl(IterObjCb cb, void* ctx) const;

    yyjson_val* val_;
};

// =========================================================================

// =========================================================================
class JsonDoc {
public:
    JsonDoc() noexcept : doc_(nullptr) {}
    explicit JsonDoc(yyjson_doc* doc) noexcept : doc_(doc) {}

    ~JsonDoc();


    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;
    JsonDoc(JsonDoc&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}
    JsonDoc& operator=(JsonDoc&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return doc_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }


    [[nodiscard]] JsonVal root() const noexcept;

    [[nodiscard]] yyjson_doc* raw() const noexcept { return doc_; }

    [[nodiscard]] bool has(std::string_view key) const noexcept;
    [[nodiscard]] bool is_null(std::string_view key) const noexcept;
    [[nodiscard]] std::string get_string(std::string_view key) const;
    [[nodiscard]] int64_t get_int(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<JsonVal> get_object(std::string_view key) const noexcept;

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

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_num()  const noexcept;
    [[nodiscard]] bool is_str()  const noexcept;
    [[nodiscard]] bool is_arr()  const noexcept;
    [[nodiscard]] bool is_obj()  const noexcept;

    /// Read-typed accessors (read the *mut* value as a primitive).
    [[nodiscard]] std::string_view as_str() const noexcept;
    [[nodiscard]] int64_t as_int()   const noexcept;
    [[nodiscard]] double  as_double() const noexcept;
    [[nodiscard]] bool    as_bool() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    /// Add / replace a key on an object.  If the key already exists it is replaced
    /// with the new value (yyjson_mut_obj_put semantics).
    void add(std::string_view key, JsonMutVal value);
    void append(JsonMutVal value);
    /// Remove a key from an object.  Returns true if the key existed.
    [[nodiscard]] bool remove(std::string_view key);
    /// Check whether an object contains the given key.
    [[nodiscard]] bool has(std::string_view key) const noexcept;
    /// Retrieve a child by key (mutable view into the same document).
    [[nodiscard]] JsonMutVal get(std::string_view key) noexcept;
    /// Retrieve a child by index (mutable, for arrays).
    [[nodiscard]] JsonMutVal at(std::size_t idx) noexcept;

    // Convenience typed setters.  These call add() with the appropriate builder.
    void set(std::string_view k, std::string_view v);
    void set(std::string_view k, const char* v);
    void set(std::string_view k, int64_t v);
    void set(std::string_view k, int v);
    void set(std::string_view k, double v);
    void set(std::string_view k, bool v);

    /// Return a newly-constructed mutable value owned by the underlying document.
    [[nodiscard]] JsonMutVal make_obj();
    [[nodiscard]] JsonMutVal make_arr();
    [[nodiscard]] JsonMutVal make_str(std::string_view s);
    [[nodiscard]] JsonMutVal make_num(int64_t n);
    [[nodiscard]] JsonMutVal make_real(double r);
    [[nodiscard]] JsonMutVal make_bool(bool b);
    [[nodiscard]] JsonMutVal make_null();

    /// Ensure that a child object with the given name exists, creating it if
    /// necessary.  Returns a mutable view of the child.
    [[nodiscard]] JsonMutVal ensure_object(std::string_view key);
    /// Ensure that a child array with the given name exists, creating it if necessary.
    [[nodiscard]] JsonMutVal ensure_array(std::string_view key);

    // Iteration helpers — the shells forward to type-erased *_impl walkers
    // (json_iter.cpp) so the inline interface never names yyjson functions.
    template<class Fn>
    void iter_obj(Fn&& fn) const {
        iter_obj_impl([](void* c, JsonMutVal k, JsonMutVal v) {
            (*static_cast<std::remove_reference_t<Fn>*>(c))(k, v);
        }, static_cast<void*>(std::addressof(fn)));
    }

    template<class Fn>
    void iter(Fn&& fn) const {
        iter_impl([](void* c, JsonMutVal v, std::size_t i) {
            (*static_cast<std::remove_reference_t<Fn>*>(c))(v, i);
        }, static_cast<void*>(std::addressof(fn)));
    }

    [[nodiscard]] yyjson_mut_val* raw() const noexcept { return val_; }
    [[nodiscard]] yyjson_mut_doc* doc() const noexcept { return doc_; }

private:
    using IterCb = void(*)(void* ctx, JsonMutVal v, std::size_t i);
    using IterObjCb = void(*)(void* ctx, JsonMutVal k, JsonMutVal v);

    void iter_impl(IterCb cb, void* ctx) const;
    void iter_obj_impl(IterObjCb cb, void* ctx) const;

    yyjson_mut_val* val_;
    yyjson_mut_doc* doc_;
};

// =========================================================================

// =========================================================================
class JsonMutDoc {
public:
    JsonMutDoc();
    ~JsonMutDoc();

    JsonMutDoc(const JsonMutDoc&) = delete;
    JsonMutDoc& operator=(const JsonMutDoc&) = delete;
    JsonMutDoc(JsonMutDoc&& other) noexcept : doc_(std::exchange(other.doc_, nullptr)) {}
    JsonMutDoc& operator=(JsonMutDoc&& other) noexcept;


    [[nodiscard]] JsonMutVal object();
    [[nodiscard]] JsonMutVal array();
    [[nodiscard]] JsonMutVal string(std::string_view s);
    [[nodiscard]] JsonMutVal number(double n);
    [[nodiscard]] JsonMutVal number(int64_t n);
    [[nodiscard]] JsonMutVal boolean(bool b);
    [[nodiscard]] JsonMutVal null();

    // Copy an immutable JsonVal into this mutable document
    [[nodiscard]] JsonMutVal copy_val(JsonVal val);

    // Parse a raw JSON string and embed it as a mutable value in this document
    [[nodiscard]] JsonMutVal raw_json(std::string_view json_str);


    void set_root(JsonMutVal val);

    /// Mutable-root accessor.  Returns a mutable view of the document's root
    /// value (or an invalid JsonMutVal when no root is set), enabling in-place
    /// reads/edits of an already-committed tree without re-parsing.
    [[nodiscard]] JsonMutVal root_mut() noexcept;


    [[nodiscard]] std::string to_string() const;


    [[nodiscard]] std::string to_pretty_string() const;

    [[nodiscard]] yyjson_mut_doc* raw() const noexcept;

private:
    yyjson_mut_doc* doc_;
};

class JsonObject {
public:
    JsonObject();

    void set(std::string_view key, std::string_view value);
    void set(std::string_view key, const char* value);
    void set(std::string_view key, int64_t value);
    void set(std::string_view key, int value);
    void set(std::string_view key, double value);
    void set(std::string_view key, bool value);

    [[nodiscard]] std::string serialize() const;

private:
    JsonMutDoc doc_;
    JsonMutVal root_;
};

class JsonArray {
public:
    JsonArray();

    void push(int64_t value);
    void push(int value);
    void push(std::string_view value);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] int64_t get_int(std::size_t index) const;
    [[nodiscard]] std::string serialize() const;

private:
    JsonMutDoc doc_;
    JsonMutVal root_;
    std::vector<int64_t> ints_;
    std::size_t size_ = 0;
};

[[nodiscard]] JsonObject object();
[[nodiscard]] JsonArray array();

// =========================================================================

// =========================================================================


/// Fluent builder over JsonMutDoc for declarative object construction.
/// Provides chainable str/boolean/num setters; doc()/root() expose the
/// underlying mutable document for nested array/object assembly, and
/// serialize() commits the root and returns the JSON text.
class JsonBuilder {
public:
    JsonBuilder();

    JsonBuilder& str(std::string_view key, std::string_view value);
    JsonBuilder& boolean(std::string_view key, bool value);
    JsonBuilder& num(std::string_view key, int64_t value);
    JsonBuilder& num(std::string_view key, double value);
    /// Adds the field only when the optional holds a value.
    JsonBuilder& opt_str(std::string_view key, const std::optional<std::string>& value);
    /// Adds a numeric count field (alias of num for collection sizes / counters).
    JsonBuilder& size(std::string_view key, std::size_t value);

    JsonMutDoc& doc() noexcept;
    JsonMutVal root() noexcept;

    std::string serialize();

private:
    JsonMutDoc doc_;
    JsonMutVal root_;
};

[[nodiscard]] Result<JsonDoc> parse(std::string_view json_str);

/// Parse the first JSON value in `json_str`, ignoring any trailing content.
/// Uses yyjson's YYJSON_READ_STOP_WHEN_DONE so concatenated JSON (e.g. pasted
/// log lines like {"a":1}{"b":2}) parses the first object instead of failing
/// the whole document on trailing content.  Mirrors parse() otherwise.
[[nodiscard]] Result<JsonDoc> parse_first(std::string_view json_str);


[[nodiscard]] Result<JsonDoc> parse_file_string(const std::string& path_string);

[[nodiscard]] Result<JsonDoc> parse_file(std::string_view path);

[[nodiscard]] Result<JsonDoc> parse_file(const std::string& path);

[[nodiscard]] Result<JsonDoc> parse_file(const char* path);

template <typename Path>
[[nodiscard]] inline Result<JsonDoc> parse_file(const Path& path) {
    return parse_file_string(path.string());
}


[[nodiscard]] std::string to_string(const JsonDoc& doc);

[[nodiscard]] std::string to_pretty_string(const JsonDoc& doc);

[[nodiscard]] std::string to_string(JsonVal val);

} // namespace cc::utils::json
