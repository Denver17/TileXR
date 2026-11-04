// Provides shared JSON types, checked field access and overflow-safe size helpers.
#pragma once
// JSON is parsed by the pinned picojson library, never by a handwritten parser.
#ifndef PICOJSON_USE_INT64
#define PICOJSON_USE_INT64
#endif
#include "picojson/picojson.h"
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace unitest {
using Json = picojson::value;
using Object = picojson::object;
using Array = picojson::array;

class UnsupportedError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline Json number(int64_t n) { return Json(n); }
inline Json object() { return Json(Object{}); }
inline void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}
inline const Json &field(const Json &v, const std::string &name) {
    require(v.is<Object>() && v.contains(name), "missing field: " + name);
    return v.get(name);
}
inline std::string string(const Json &v) {
    require(v.is<std::string>(), "expected JSON string");
    return v.get<std::string>();
}
inline int64_t integer(const Json &v, int64_t low, int64_t high) {
    require(v.is<int64_t>(), "expected JSON integer");
    const auto n = v.get<int64_t>();
    require(n >= low && n <= high, "integer outside supported range");
    return n;
}
inline int64_t integer(const Json &v, const std::string &key, int64_t low, int64_t high) {
    return integer(field(v, key), low, high);
}
inline size_t product(size_t a, size_t b) {
    require(b == 0 || a <= std::numeric_limits<size_t>::max() / b, "size overflow");
    return a * b;
}
inline void put(Json &v, const std::string &key, Json value) {
    require(v.is<Object>(), "expected object");
    v.get<Object>()[key] = std::move(value);
}
} // namespace unitest
