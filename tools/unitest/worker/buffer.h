// Declares checked device allocation, Host/device transfers and explicit release.
// Buffer ownership belongs to Session; callers must synchronize before release.
#pragma once
#include "json.h"
#include <acl/acl.h>
#include <tilexr_types.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace unitest {
inline void check(int code, const char *operation) {
    if (code == TileXR::TILEXR_ERROR_NOT_SUPPORT)
        throw UnsupportedError(std::string(operation) + " is unsupported");
    if (code) throw std::runtime_error(std::string(operation) + " returned " + std::to_string(code));
}
class Buffer {
public:
    explicit Buffer(size_t bytes);
    ~Buffer();
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    void *data() const { return data_; }
    void upload(const void *data, size_t bytes);
    template<class T> void upload(const std::vector<T> &data) {
        upload(data.data(), data.size()*sizeof(T));
    }
    template<class T> std::vector<T> read(size_t count) const {
        if (count > bytes_/sizeof(T)) throw std::runtime_error("output exceeds capacity");
        std::vector<T> out(count);
        if (count) check(aclrtMemcpy(out.data(), count*sizeof(T), data_, count*sizeof(T),
            ACL_MEMCPY_DEVICE_TO_HOST), "D2H");
        return out;
    }
    void poison();
    void release();
private:
    void *data_ = nullptr;
    size_t bytes_;
};
} // namespace unitest
