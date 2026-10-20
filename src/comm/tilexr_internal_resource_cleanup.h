/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
#ifndef TILEXR_INTERNAL_RESOURCE_CLEANUP_H
#define TILEXR_INTERNAL_RESOURCE_CLEANUP_H
namespace TileXR {
// Kept as a testable ordering boundary: local absence/success must still join
// the common status phase after peers drain their optional resources.
template<class Communicator>
inline int FinalizeInternalResourceTogether(Communicator &comm)
{
    const int localStatus = comm.FinalizeInternalResource();
    return comm.AgreeInternalStatus(localStatus);
}
}
#endif
