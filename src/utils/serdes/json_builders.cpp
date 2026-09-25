// Implementation unit for cc.utils.json — JsonObject / JsonArray / JsonBuilder
// and the free object()/array() factories (RFC 0001 Phase C batch 10). These
// compose only the JsonMutDoc / JsonMutVal wrappers and make no yyjson calls,
// so this unit has an empty global module fragment.
module cc.utils.json;

import std;

namespace cc::utils::json {

JsonObject::JsonObject() : root_(doc_.object()) { doc_.set_root(root_); }

void JsonObject::set(std::string_view key, std::string_view value) { root_.add(key, doc_.string(value)); }
void JsonObject::set(std::string_view key, const char* value) { root_.add(key, doc_.string(value ? std::string_view(value) : std::string_view{})); }
void JsonObject::set(std::string_view key, std::int64_t value) { root_.add(key, doc_.number(value)); }
void JsonObject::set(std::string_view key, int value) { root_.add(key, doc_.number(static_cast<std::int64_t>(value))); }
void JsonObject::set(std::string_view key, double value) { root_.add(key, doc_.number(value)); }
void JsonObject::set(std::string_view key, bool value) { root_.add(key, doc_.boolean(value)); }

std::string JsonObject::serialize() const { return doc_.to_string(); }

JsonArray::JsonArray() : root_(doc_.array()) { doc_.set_root(root_); }

void JsonArray::push(std::int64_t value) { root_.append(doc_.number(value)); ints_.push_back(value); ++size_; }
void JsonArray::push(int value) { push(static_cast<std::int64_t>(value)); }
void JsonArray::push(std::string_view value) { root_.append(doc_.string(value)); ++size_; }

std::size_t JsonArray::size() const noexcept { return size_; }
std::int64_t JsonArray::get_int(std::size_t index) const {
    return index < ints_.size() ? ints_[index] : 0;
}
std::string JsonArray::serialize() const { return doc_.to_string(); }

JsonObject object() { return JsonObject{}; }
JsonArray array() { return JsonArray{}; }

JsonBuilder::JsonBuilder() : root_(doc_.object()) {}

JsonBuilder& JsonBuilder::str(std::string_view key, std::string_view value) {
    root_.add(key, doc_.string(value));
    return *this;
}
JsonBuilder& JsonBuilder::boolean(std::string_view key, bool value) {
    root_.add(key, doc_.boolean(value));
    return *this;
}
JsonBuilder& JsonBuilder::num(std::string_view key, std::int64_t value) {
    root_.add(key, doc_.number(value));
    return *this;
}
JsonBuilder& JsonBuilder::num(std::string_view key, double value) {
    root_.add(key, doc_.number(value));
    return *this;
}
JsonBuilder& JsonBuilder::opt_str(std::string_view key, const std::optional<std::string>& value) {
    if (value) root_.add(key, doc_.string(*value));
    return *this;
}
JsonBuilder& JsonBuilder::size(std::string_view key, std::size_t value) {
    root_.add(key, doc_.number(static_cast<std::int64_t>(value)));
    return *this;
}

JsonMutDoc& JsonBuilder::doc() noexcept { return doc_; }
JsonMutVal JsonBuilder::root() noexcept { return root_; }

std::string JsonBuilder::serialize() {
    doc_.set_root(root_);
    return doc_.to_string();
}

} // namespace cc::utils::json
