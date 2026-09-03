#include <cstdint>
#include <iostream>

#include "tilexr_api.h"
#include "tilexr_types.h"
#include "tilexr_internal.h"

namespace {

int g_failures = 0;
int g_destroyCalls = 0;
int g_retryDestroyCalls = 0;

int DestroyTestResource(TileXRCommPtr, void *resource)
{
    if (resource != reinterpret_cast<void *>(0x1234)) {
        ++g_failures;
    }
    ++g_destroyCalls;
    return TileXR::TILEXR_SUCCESS;
}

int DestroyRetryResource(TileXRCommPtr, void *)
{
    ++g_retryDestroyCalls;
    return g_retryDestroyCalls == 1 ?
        TileXR::TILEXR_ERROR_INTERNAL : TileXR::TILEXR_SUCCESS;
}

void CheckStatus(const char *label, int actual, int expected)
{
    if (actual != expected) {
        std::cerr << label << " returned " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

void CheckMagic(const char *label, int64_t actual, int64_t expected)
{
    if (actual != expected) {
        std::cerr << label << " wrote magic " << actual << ", expected " << expected << std::endl;
        ++g_failures;
    }
}

} // namespace

int main()
{
    int64_t magic = -1;
    CheckStatus("TileXRCommNextMagic(nullptr, &magic)",
                TileXRCommNextMagic(nullptr, &magic),
                TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);
    CheckMagic("TileXRCommNextMagic(nullptr, &magic)", magic, -1);

    TileXRCommPtr comm = nullptr;
    CheckStatus("TileXRCommInit(0, 1, &comm)",
                TileXRCommInit(0, 1, &comm),
                TileXR::TILEXR_SUCCESS);
    if (comm == nullptr) {
        std::cerr << "TileXRCommInit returned success but left comm null" << std::endl;
        return 1;
    }

    CheckStatus("local agreement without socket preserves success",
                TileXRCommAgreeInternalStatus(
                    comm, TileXR::TILEXR_SUCCESS),
                TileXR::TILEXR_SUCCESS);
    CheckStatus("local agreement without socket preserves failure",
                TileXRCommAgreeInternalStatus(
                    comm, TileXR::TILEXR_ERROR_INTERNAL),
                TileXR::TILEXR_ERROR_INTERNAL);

    CheckStatus("TileXRCommNextMagic(comm, nullptr)",
                TileXRCommNextMagic(comm, nullptr),
                TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    magic = -1;
    CheckStatus("first TileXRCommNextMagic(comm, &magic)",
                TileXRCommNextMagic(comm, &magic),
                TileXR::TILEXR_SUCCESS);
    CheckMagic("first TileXRCommNextMagic(comm, &magic)", magic, 1);

    magic = -1;
    CheckStatus("second TileXRCommNextMagic(comm, &magic)",
                TileXRCommNextMagic(comm, &magic),
                TileXR::TILEXR_SUCCESS);
    CheckMagic("second TileXRCommNextMagic(comm, &magic)", magic, 2);

    void *resource = reinterpret_cast<void *>(0x1234);
    CheckStatus("TileXRCommAttachInternalResource",
                TileXRCommAttachInternalResource(
                    comm, resource, DestroyTestResource),
                TileXR::TILEXR_SUCCESS);
    void *observed = nullptr;
    CheckStatus("TileXRCommGetInternalResource",
                TileXRCommGetInternalResource(comm, &observed),
                TileXR::TILEXR_SUCCESS);
    if (observed != resource) {
        std::cerr << "internal resource lookup returned the wrong pointer" << std::endl;
        ++g_failures;
    }
    CheckStatus("duplicate TileXRCommAttachInternalResource",
                TileXRCommAttachInternalResource(
                    comm, resource, DestroyTestResource),
                TileXR::TILEXR_ERROR_PARA_CHECK_FAIL);

    CheckStatus("TileXRCommDestroy(comm)",
                TileXRCommDestroy(comm),
                TileXR::TILEXR_SUCCESS);
    if (g_destroyCalls != 1) {
        std::cerr << "internal resource destroy callback count "
                  << g_destroyCalls << ", expected 1" << std::endl;
        ++g_failures;
    }

    TileXRCommPtr retryComm = nullptr;
    CheckStatus("TileXRCommInit retry comm",
                TileXRCommInit(0, 1, &retryComm),
                TileXR::TILEXR_SUCCESS);
    CheckStatus("attach retry resource",
                TileXRCommAttachInternalResource(
                    retryComm, reinterpret_cast<void *>(0x5678),
                    DestroyRetryResource),
                TileXR::TILEXR_SUCCESS);
    CheckStatus("destroy retry resource",
                TileXRCommDestroy(retryComm),
                TileXR::TILEXR_ERROR_INTERNAL);
    if (g_retryDestroyCalls != 1) {
        std::cerr << "failed internal resource destroy callback count "
                  << g_retryDestroyCalls << ", expected 1" << std::endl;
        ++g_failures;
    }
    CheckStatus("retry TileXRCommDestroy",
                TileXRCommDestroy(retryComm),
                TileXR::TILEXR_SUCCESS);
    if (g_retryDestroyCalls != 2) {
        std::cerr << "retrying internal resource destroy callback count "
                  << g_retryDestroyCalls << ", expected 2" << std::endl;
        ++g_failures;
    }
    return g_failures == 0 ? 0 : 1;
}
