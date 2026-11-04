// Declares the operator launch/validation interface and adapter factory.
// Adapters borrow Session-owned device buffers; Session controls their release.
#pragma once
#include "json.h"
#include <memory>
namespace unitest {
class Session;
struct Case;
class Adapter {
public:
    virtual ~Adapter() = default;
    virtual void reset() = 0;
    virtual void launch() = 0;
    virtual Json validate() = 0;
};
std::unique_ptr<Adapter> make_adapter(const Case &test_case, Session &session);
} // namespace unitest
