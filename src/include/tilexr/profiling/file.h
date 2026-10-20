/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
#ifndef TILEXR_PROFILING_FILE_H
#define TILEXR_PROFILING_FILE_H
#include "host.h"
#include <cerrno>
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
namespace TileXR { namespace Profiling {
namespace Detail {
#if defined(__linux__)
struct LinuxFileOps {
    static int Open(const char *p) { return ::open(p, O_WRONLY | O_CREAT | O_EXCL, 0600); }
    static int RandomOpen() { return ::open("/dev/urandom", O_RDONLY); }
    static ssize_t Write(int fd, const void *p, size_t n) { return ::write(fd,p,n); }
    static ssize_t Read(int fd, void *p, size_t n) { return ::read(fd,p,n); }
    static int Sync(int fd) { return ::fsync(fd); }
    static int Close(int fd) { return ::close(fd); }
    static int Link(const char *a, const char *b) { return ::link(a,b); }
    static int Unlink(const char *p) { return ::unlink(p); }
};
template<class Ops> inline Status PublishLinux(const std::string &path, const std::vector<uint8_t> &bytes)
{
    if (path.empty() || path.find('\0') != std::string::npos) return {StatusCode::InvalidArgument,"empty or NUL-containing output path"};
    std::array<uint8_t,16> random {};
    int randomFd = Ops::RandomOpen();
    if (randomFd < 0) return {StatusCode::Io,"cannot obtain unique temporary filename"};
    size_t got = 0; bool randomOk = true;
    while (got < random.size()) {
        const ssize_t n = Ops::Read(randomFd,random.data()+got,random.size()-got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { randomOk = false; break; } got += size_t(n);
    }
    if (Ops::Close(randomFd) != 0) randomOk = false;
    if (!randomOk) return {StatusCode::Io,"cannot read temporary filename entropy"};
    const char hex[] = "0123456789abcdef"; std::string temp = path + ".tmp.";
    for (uint8_t b : random) { temp += hex[b>>4]; temp += hex[b&15]; }
    const int fd = Ops::Open(temp.c_str());
    if (fd < 0) return {StatusCode::Io,"cannot exclusively create temporary records file: errno="+std::to_string(errno)};
    size_t written = 0; bool success = true;
    while (written < bytes.size()) {
        const ssize_t n = Ops::Write(fd,bytes.data()+written,bytes.size()-written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { success = false; break; } written += size_t(n);
    }
    if (success && Ops::Sync(fd) != 0) success = false;
    // A failed close is not retried: Linux may already have released the fd.
    if (Ops::Close(fd) != 0) success = false;
    if (!success) { Ops::Unlink(temp.c_str()); return {StatusCode::Io,"records write/sync/close failed"}; }
    if (Ops::Link(temp.c_str(),path.c_str()) != 0) {
        const int error = errno; Ops::Unlink(temp.c_str());
        return {StatusCode::Io,"records publication failed without replacing target: errno="+std::to_string(error)};
    }
    if (Ops::Unlink(temp.c_str()) != 0)
        return {StatusCode::PublishedCleanupWarning,"records published; temporary file cleanup failed: "+temp};
    return {};
}
#endif
}
inline Status PublishRecords(const std::string &path, const std::vector<uint8_t> &bytes)
{
#if defined(__linux__)
    return Detail::PublishLinux<Detail::LinuxFileOps>(path, bytes);
#else
    (void)path; (void)bytes; return {StatusCode::Io,"records publication requires Linux O_EXCL and hard links"};
#endif
}
inline Status ExportSnapshot(const CaptureSnapshot &snapshot, const std::string &path)
{
    std::vector<uint8_t> bytes; Status s = EncodeV1(snapshot,&bytes);
    return s ? PublishRecords(path,bytes) : s;
}
}}
#endif
