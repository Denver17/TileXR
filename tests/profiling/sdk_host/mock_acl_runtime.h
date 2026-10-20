#include <tilexr/profiling.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <type_traits>
#if defined(__linux__)
#include <unistd.h>
#endif
#if defined(_WIN32)
#include <malloc.h>
#endif
#define CHECK(x) do { if(!(x)) { std::cerr<<__LINE__<<": " #x "\n"; std::abort(); } } while(0)
using namespace TileXR::Profiling;
namespace Mock {
std::map<std::string,int> calls;
std::string failure; int failureNth=1;
bool failFree=false;
std::set<void *> allocations;
aclrtContext context=reinterpret_cast<void *>(1);
aclrtStream expectedStream=reinterpret_cast<void *>(2);
bool Fail(const char *op) { return ++calls[op]==failureNth && failure==op; }
void Reset(const std::string &op="",int nth=1) { calls.clear(); failure=op; failureNth=nth; failFree=false; context=reinterpret_cast<void *>(1); }
}
aclError aclrtGetCurrentContext(aclrtContext *out) { if(Mock::Fail("context"))return 71; *out=Mock::context;return 0; }
aclError aclrtMalloc(void **out,size_t bytes,int) {
 if(Mock::Fail("malloc"))return 72;
#if defined(_WIN32)
 *out=_aligned_malloc(bytes,512);
#else
 CHECK(posix_memalign(out,512,bytes)==0);
#endif
 CHECK(*out); CHECK(Mock::allocations.insert(*out).second);return 0;
}
aclError aclrtMemset(void *p,size_t max,int value,size_t bytes) { if(Mock::Fail("memset"))return 73; CHECK(bytes<=max); std::memset(p,value,bytes);return 0; }
aclError aclrtMemcpy(void *out,size_t max,const void *in,size_t bytes,int kind) {
 if(Mock::Fail(kind==ACL_MEMCPY_HOST_TO_DEVICE?"h2d":"d2h"))return 74; CHECK(bytes<=max);std::memcpy(out,in,bytes);return 0;
}
aclError aclrtSynchronizeStream(aclrtStream stream) { if(Mock::expectedStream) CHECK(stream==Mock::expectedStream); return Mock::Fail("sync")?75:0; }
aclError aclrtFree(void *p) { if(Mock::Fail("free") || Mock::failFree)return 76; CHECK(Mock::allocations.erase(p)==1);
#if defined(_WIN32)
 _aligned_free(p);
#else
 std::free(p);
#endif
 return 0; }
