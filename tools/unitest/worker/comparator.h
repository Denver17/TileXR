// Compares output elements exactly and builds JSON checks with error counts
// and a single first-mismatch value pair to keep diagnostics bounded.
#pragma once
#include "json.h"
#include <vector>
namespace unitest {
// Record all mismatches but only the first value pair to keep reports bounded.
template<class T>
Json compare(const std::string &name, const std::vector<T> &actual, const std::vector<T> &expected) {
    require(actual.size() >= expected.size(), "output buffer too short: " + name);
    Json result = object();
    put(result, "output", Json(name));
    int64_t errors = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] == expected[i]) continue;
        if (errors++ == 0) {
            put(result, "index", number(static_cast<int64_t>(i)));
            put(result, "expected", number(static_cast<int64_t>(expected[i])));
            put(result, "actual", number(static_cast<int64_t>(actual[i])));
        }
    }
    put(result, "errors", number(errors));
    put(result, "checked_elements", number(static_cast<int64_t>(expected.size())));
    put(result, "status", Json(errors ? "failed" : "passed"));
    return result;
}
} // namespace unitest
