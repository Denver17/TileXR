/*
 * Copyright (c) 2024-2026 TileXR Project
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "acl/acl.h"
#include "tilexr_collectives.h"
#include "../common/int32_pattern.h"

namespace {

constexpr int64_t kMaxHostBufferBytes = 1LL << 30;

enum class CollectiveOp {
    ALLGATHER,
    ALLTOALL,
    ALLREDUCE,
    REDUCESCATTER,
    BROADCAST,
    NOOP,
};

enum class CommMode {
    LOCAL,
    SOCKET,
    SHARED_QP,
};

struct DataTypeInfo {
    TileXR::TileXRDataType type = TileXR::TILEXR_DATA_TYPE_INT32;
    std::string name = "int32";
    size_t bytes = sizeof(int32_t);
};

struct Options {
    CollectiveOp op = CollectiveOp::ALLGATHER;
    int64_t minBytes = 4;
    int64_t maxBytes = 4096;
    double stepFactor = 2.0;
    int iters = 20;
    int warmupIters = 5;
    DataTypeInfo dtype;
    int rankSize = 2;
    int rank = 0;
    int firstNpu = 0;
    int deviceIdOverride = -1;
    CommMode commMode = CommMode::LOCAL;
    int commDomain = 61;
    bool check = true;
    std::string csvPath;
    double minAlgBw = -1.0;
    double maxLatencyUs = -1.0;
    bool batchTiming = false;
};

struct Measurement {
    double avgUs = 0.0;
    double minUs = 0.0;
    double maxUs = 0.0;
    bool hasLatencyRange = true;
};

struct Row {
    std::string op;
    std::string dtype;
    int ranks = 0;
    int64_t bytes = 0;
    int64_t count = 0;
    int iters = 0;
    double algBw = 0.0;
    double busBw = 0.0;
    double avgUs = 0.0;
    double minUs = 0.0;
    double maxUs = 0.0;
    bool hasLatencyRange = true;
    int errors = 0;
};

using TileXRCollectivesTest::CanUseCollisionFreeInt32Pattern;
using TileXRCollectivesTest::ExpectedAllGatherValue;
using TileXRCollectivesTest::ExpectedAllToAllValue;

bool IsReductionOp(CollectiveOp op)
{
    return op == CollectiveOp::ALLREDUCE || op == CollectiveOp::REDUCESCATTER;
}

std::string OpName(CollectiveOp op)
{
    switch (op) {
        case CollectiveOp::ALLGATHER:
            return "allgather";
        case CollectiveOp::ALLTOALL:
            return "alltoall";
        case CollectiveOp::ALLREDUCE:
            return "allreduce";
        case CollectiveOp::REDUCESCATTER:
            return "reducescatter";
        case CollectiveOp::BROADCAST:
            return "broadcast";
        case CollectiveOp::NOOP:
            return "noop";
    }
    return "unknown";
}

std::string CommModeName(CommMode mode)
{
    switch (mode) {
        case CommMode::LOCAL:
            return "local";
        case CommMode::SOCKET:
            return "socket";
        case CommMode::SHARED_QP:
            return "shared-qp";
    }
    return "unknown";
}

int32_t ExpectedAllReduceSum(int rankSize, int64_t index)
{
    int64_t sum = 0;
    for (int rank = 0; rank < rankSize; ++rank) {
        sum += ExpectedAllGatherValue(rankSize, rank, index);
    }
    return static_cast<int32_t>(sum);
}

int32_t ExpectedReduceScatterSum(int rankSize, int rank, int64_t recvCount, int64_t index)
{
    const int64_t globalIndex = static_cast<int64_t>(rank) * recvCount + index;
    int64_t sum = 0;
    for (int srcRank = 0; srcRank < rankSize; ++srcRank) {
        sum += ExpectedAllGatherValue(rankSize, srcRank, globalIndex);
    }
    return static_cast<int32_t>(sum);
}

uint8_t PatternByte(int rank, int peer, int64_t byteIndex)
{
    return static_cast<uint8_t>((rank * 37 + peer * 13 + byteIndex) & 0xff);
}

void StoreInt32(std::vector<uint8_t> &buffer, int64_t index, int32_t value)
{
    std::memcpy(buffer.data() + static_cast<size_t>(index) * sizeof(int32_t), &value, sizeof(value));
}

int32_t LoadInt32(const std::vector<uint8_t> &buffer, int64_t index)
{
    int32_t value = 0;
    std::memcpy(&value, buffer.data() + static_cast<size_t>(index) * sizeof(int32_t), sizeof(value));
    return value;
}

void PrintUsage(const char *program)
{
    std::cerr
        << "Usage: " << program << " [options]\n"
        << "  --op allgather|alltoall|allreduce|reducescatter|broadcast|noop\n"
        << "  Message-size semantics: allgather/allreduce/broadcast: count * dtype_size; "
           "alltoall/reducescatter: count * rank_size * dtype_size\n"
        << "  --min-bytes N --max-bytes N --step-factor F\n"
        << "  --iters N --warmup-iters N [--batch-timing 0|1]\n"
        << "  --datatype int8|int16|int32|int64|fp16|fp32|bf16\n"
        << "  --rank-size N --rank R --first-npu D\n"
        << "  [--device-id D] [--comm-mode local|socket|shared-qp] [--comm-domain N]\n"
        << "  --check 0|1 [--csv path]\n"
        << "  [--min-algbw GB/s] [--max-latency-us us]\n";
}

bool ParseBool(const std::string &value, bool &out)
{
    if (value == "1" || value == "true" || value == "yes") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no") {
        out = false;
        return true;
    }
    return false;
}

bool ParseDataType(const std::string &value, DataTypeInfo &info)
{
    if (value == "int8") {
        info = DataTypeInfo{TileXR::TILEXR_DATA_TYPE_INT8, "int8", 1};
    } else if (value == "int16") {
        info = DataTypeInfo{TileXR::TILEXR_DATA_TYPE_INT16, "int16", 2};
    } else if (value == "int32") {
        info = DataTypeInfo{TileXR::TILEXR_DATA_TYPE_INT32, "int32", 4};
    } else if (value == "int64") {
        info = DataTypeInfo{TileXR::TILEXR_DATA_TYPE_INT64, "int64", 8};
    } else if (value == "fp16") {
        info = DataTypeInfo{TileXR::TILEXR_DATA_TYPE_FP16, "fp16", 2};
    } else if (value == "fp32") {
        info = DataTypeInfo{TileXR::TILEXR_DATA_TYPE_FP32, "fp32", 4};
    } else if (value == "bf16" || value == "bfp16") {
        info = DataTypeInfo{TileXR::TILEXR_DATA_TYPE_BFP16, "bf16", 2};
    } else {
        return false;
    }
    return true;
}

bool ParseCommMode(const std::string &value, CommMode &mode)
{
    if (value == "local") {
        mode = CommMode::LOCAL;
        return true;
    }
    if (value == "socket") {
        mode = CommMode::SOCKET;
        return true;
    }
    if (value == "shared-qp") {
        mode = CommMode::SHARED_QP;
        return true;
    }
    return false;
}

bool ParseInt64(const std::string &text, int64_t &out)
{
    char *end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int64_t>(value);
    return true;
}

bool ParseInt(const std::string &text, int &out)
{
    int64_t value = 0;
    if (!ParseInt64(text, value) || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool ParseDouble(const std::string &text, double &out)
{
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool CheckedMulInt64(int64_t lhs, int64_t rhs, int64_t &out)
{
    if (lhs < 0 || rhs < 0) {
        return false;
    }
    if (lhs != 0 && rhs > std::numeric_limits<int64_t>::max() / lhs) {
        return false;
    }
    out = lhs * rhs;
    return true;
}

bool CheckedBytesForElements(int64_t elements, size_t elementBytes, int64_t &bytes)
{
    if (elementBytes > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    return CheckedMulInt64(elements, static_cast<int64_t>(elementBytes), bytes) &&
        static_cast<uint64_t>(bytes) <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

bool FitsHostBufferLimit(int64_t bytes)
{
    return bytes > 0 && bytes <= kMaxHostBufferBytes;
}

bool ComputeMessageSizes(const Options &options, int64_t count, int64_t &sendElements, int64_t &recvElements,
                         int64_t &sendBytes, int64_t &recvBytes)
{
    if (count <= 0) {
        return false;
    }
    const int64_t rankSize = static_cast<int64_t>(options.rankSize);
    sendElements = count;
    recvElements = count;
    switch (options.op) {
        case CollectiveOp::ALLGATHER:
            if (!CheckedMulInt64(count, rankSize, recvElements)) {
                return false;
            }
            break;
        case CollectiveOp::ALLTOALL:
            if (!CheckedMulInt64(count, rankSize, sendElements) ||
                !CheckedMulInt64(count, rankSize, recvElements)) {
                return false;
            }
            break;
        case CollectiveOp::ALLREDUCE:
            break;
        case CollectiveOp::REDUCESCATTER:
            if (!CheckedMulInt64(count, rankSize, sendElements)) {
                return false;
            }
            break;
        case CollectiveOp::BROADCAST:
            break;
        case CollectiveOp::NOOP:
            break;
    }
    return CheckedBytesForElements(sendElements, options.dtype.bytes, sendBytes) &&
        CheckedBytesForElements(recvElements, options.dtype.bytes, recvBytes) &&
        FitsHostBufferLimit(sendBytes) && FitsHostBufferLimit(recvBytes);
}

int64_t MessageSizeDivisor(const Options &options)
{
    const int64_t divisor = (options.op == CollectiveOp::ALLTOALL ||
                             options.op == CollectiveOp::REDUCESCATTER) ?
        static_cast<int64_t>(options.rankSize) * static_cast<int64_t>(options.dtype.bytes) :
        static_cast<int64_t>(options.dtype.bytes);
    return divisor;
}

bool ValidateMaxMessageSize(const Options &options)
{
    int64_t count = options.maxBytes / MessageSizeDivisor(options);
    if (count <= 0) {
        count = 1;
    }
    int64_t validationCount = count;
    if (options.op == CollectiveOp::REDUCESCATTER &&
        !CheckedMulInt64(count, static_cast<int64_t>(options.rankSize), validationCount)) {
        std::cerr << "ERROR: message size too large" << std::endl;
        return false;
    }
    if (options.check && options.dtype.name == "int32" &&
        options.op != CollectiveOp::NOOP &&
        !CanUseCollisionFreeInt32Pattern(options.rankSize, validationCount)) {
        std::cerr << "ERROR: message size is too large for collision-free INT32 validation" << std::endl;
        return false;
    }
    int64_t sendElements = 0;
    int64_t recvElements = 0;
    int64_t sendBytes = 0;
    int64_t recvBytes = 0;
    if (!ComputeMessageSizes(options, count, sendElements, recvElements, sendBytes, recvBytes)) {
        std::cerr << "ERROR: message size too large" << std::endl;
        return false;
    }
    (void)sendElements;
    (void)recvElements;
    (void)sendBytes;
    (void)recvBytes;
    return true;
}

bool AdvanceBytes(int64_t current, double factor, int64_t maxBytes, int64_t &next)
{
    const double scaled = static_cast<double>(current) * factor;
    if (!std::isfinite(scaled) || scaled > static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    const int64_t candidate = static_cast<int64_t>(scaled);
    if (candidate <= current) {
        if (current == std::numeric_limits<int64_t>::max()) {
            return false;
        }
        next = current + 1;
    } else {
        next = candidate;
    }
    if (next < current) {
        return false;
    }
    if (next > maxBytes) {
        next = maxBytes;
    }
    return true;
}

bool ParseOptions(int argc, char **argv, Options &options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const std::string &name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: missing value for " << name << std::endl;
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--op") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            const std::string op(value);
            if (op == "allgather") {
                options.op = CollectiveOp::ALLGATHER;
            } else if (op == "alltoall") {
                options.op = CollectiveOp::ALLTOALL;
            } else if (op == "allreduce") {
                options.op = CollectiveOp::ALLREDUCE;
            } else if (op == "reducescatter") {
                options.op = CollectiveOp::REDUCESCATTER;
            } else if (op == "broadcast") {
                options.op = CollectiveOp::BROADCAST;
            } else if (op == "noop") {
                options.op = CollectiveOp::NOOP;
            } else {
                std::cerr
                    << "ERROR: --op must be allgather, alltoall, allreduce, reducescatter, broadcast, or noop"
                    << std::endl;
                return false;
            }
        } else if (arg == "--min-bytes") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseInt64(value, options.minBytes)) {
                std::cerr << "ERROR: invalid --min-bytes" << std::endl;
                return false;
            }
        } else if (arg == "--max-bytes") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseInt64(value, options.maxBytes)) {
                std::cerr << "ERROR: invalid --max-bytes" << std::endl;
                return false;
            }
        } else if (arg == "--step-factor") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseDouble(value, options.stepFactor)) {
                std::cerr << "ERROR: invalid --step-factor" << std::endl;
                return false;
            }
        } else if (arg == "--iters") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseInt(value, options.iters)) {
                std::cerr << "ERROR: invalid --iters" << std::endl;
                return false;
            }
        } else if (arg == "--warmup-iters") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseInt(value, options.warmupIters)) {
                std::cerr << "ERROR: invalid --warmup-iters" << std::endl;
                return false;
            }
        } else if (arg == "--batch-timing") {
            const char *value = requireValue(arg);
            if (value == nullptr || !ParseBool(value, options.batchTiming)) {
                std::cerr << "ERROR: --batch-timing must be 0 or 1" << std::endl;
                return false;
            }
        } else if (arg == "--datatype") {
            const char *value = requireValue(arg);
            if (value == nullptr || !ParseDataType(value, options.dtype)) {
                std::cerr << "ERROR: unsupported --datatype" << std::endl;
                return false;
            }
        } else if (arg == "--rank-size") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseInt(value, options.rankSize)) {
                std::cerr << "ERROR: invalid --rank-size" << std::endl;
                return false;
            }
        } else if (arg == "--rank") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseInt(value, options.rank)) {
                std::cerr << "ERROR: invalid --rank" << std::endl;
                return false;
            }
        } else if (arg == "--first-npu") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseInt(value, options.firstNpu)) {
                std::cerr << "ERROR: invalid --first-npu" << std::endl;
                return false;
            }
        } else if (arg == "--device-id") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseInt(value, options.deviceIdOverride)) {
                std::cerr << "ERROR: invalid --device-id" << std::endl;
                return false;
            }
        } else if (arg == "--comm-mode") {
            const char *value = requireValue(arg);
            if (value == nullptr || !ParseCommMode(value, options.commMode)) {
                std::cerr << "ERROR: --comm-mode must be local, socket, or shared-qp" << std::endl;
                return false;
            }
        } else if (arg == "--comm-domain") {
            const char *value = requireValue(arg);
            if (value == nullptr || !ParseInt(value, options.commDomain)) {
                std::cerr << "ERROR: invalid --comm-domain" << std::endl;
                return false;
            }
        } else if (arg == "--check") {
            const char *value = requireValue(arg);
            if (value == nullptr || !ParseBool(value, options.check)) {
                std::cerr << "ERROR: --check must be 0 or 1" << std::endl;
                return false;
            }
        } else if (arg == "--csv") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            options.csvPath = value;
        } else if (arg == "--min-algbw") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseDouble(value, options.minAlgBw)) {
                std::cerr << "ERROR: invalid --min-algbw" << std::endl;
                return false;
            }
        } else if (arg == "--max-latency-us") {
            const char *value = requireValue(arg);
            if (value == nullptr) {
                return false;
            }
            if (!ParseDouble(value, options.maxLatencyUs)) {
                std::cerr << "ERROR: invalid --max-latency-us" << std::endl;
                return false;
            }
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "ERROR: unknown argument " << arg << std::endl;
            return false;
        }
    }

    if (options.minBytes <= 0 || options.maxBytes < options.minBytes || options.stepFactor <= 1.0 ||
        options.iters <= 0 || options.warmupIters < 0 || options.rankSize <= 0 ||
        options.rank < 0 || options.rank >= options.rankSize || options.firstNpu < 0 ||
        options.deviceIdOverride < -1 || options.commDomain < 0) {
        std::cerr << "ERROR: invalid option value" << std::endl;
        return false;
    }
    const char *profilingRecordsPath = std::getenv("TILEXR_PROFILING_RECORDS_PATH");
    if (options.batchTiming && profilingRecordsPath != nullptr && profilingRecordsPath[0] != '\0') {
        std::cerr << "ERROR: --batch-timing does not support profiling" << std::endl;
        return false;
    }
    if (options.check && options.dtype.name != "int32" && IsReductionOp(options.op)) {
        std::cerr << "ERROR: --check=1 for allreduce/reducescatter requires --datatype int32" << std::endl;
        return false;
    }
    if (!ValidateMaxMessageSize(options)) {
        return false;
    }
    return true;
}

bool CheckAcl(int rank, const std::string &step, aclError ret)
{
    if (ret == ACL_SUCCESS) {
        return true;
    }
    std::cerr << "[rank " << rank << "] ERROR: " << step << " failed, ret=" << ret << std::endl;
    return false;
}

bool CheckTileXR(int rank, const std::string &step, int ret)
{
    if (ret == TileXR::TILEXR_SUCCESS) {
        return true;
    }
    std::cerr << "[rank " << rank << "] ERROR: " << step << " failed, ret=" << ret << std::endl;
    return false;
}

bool CallCollective(const Options &options, void *sendBuf, void *recvBuf, int64_t count, TileXRCommPtr comm,
                    aclrtStream stream)
{
    switch (options.op) {
        case CollectiveOp::ALLGATHER:
            return CheckTileXR(options.rank, "TileXRAllGather",
                TileXRAllGather(sendBuf, recvBuf, count, options.dtype.type, comm, stream));
        case CollectiveOp::ALLTOALL:
            return CheckTileXR(options.rank, "TileXRAllToAll",
                TileXRAllToAll(sendBuf, recvBuf, count, options.dtype.type, comm, stream));
        case CollectiveOp::ALLREDUCE:
            return CheckTileXR(options.rank, "TileXRAllReduce",
                TileXRAllReduce(sendBuf, recvBuf, count, options.dtype.type,
                                TileXR::TILEXR_REDUCE_SUM, comm, stream));
        case CollectiveOp::REDUCESCATTER:
            return CheckTileXR(options.rank, "TileXRReduceScatter",
                TileXRReduceScatter(sendBuf, recvBuf, count, options.dtype.type,
                                    TileXR::TILEXR_REDUCE_SUM, comm, stream));
        case CollectiveOp::BROADCAST:
            return CheckTileXR(options.rank, "TileXRBroadcast",
                TileXRBroadcast(sendBuf, count, options.dtype.type, 0, comm, stream));
        case CollectiveOp::NOOP:
            (void)sendBuf;
            (void)recvBuf;
            (void)count;
            (void)comm;
            (void)stream;
            return true;
    }
    return false;
}

bool FillPattern(const Options &options, int64_t count, std::vector<uint8_t> &send, std::vector<uint8_t> &recv)
{
    if (options.dtype.name == "int32") {
        switch (options.op) {
            case CollectiveOp::ALLGATHER:
            case CollectiveOp::ALLREDUCE:
                for (int64_t i = 0; i < count; ++i) {
                    StoreInt32(send, i, ExpectedAllGatherValue(options.rankSize, options.rank, i));
                }
                break;
            case CollectiveOp::ALLTOALL:
                for (int dst = 0; dst < options.rankSize; ++dst) {
                    for (int64_t i = 0; i < count; ++i) {
                        StoreInt32(send, static_cast<int64_t>(dst) * count + i,
                            ExpectedAllToAllValue(options.rankSize, options.rank, dst, i));
                    }
                }
                break;
            case CollectiveOp::REDUCESCATTER:
                for (int64_t i = 0; i < count * static_cast<int64_t>(options.rankSize); ++i) {
                    StoreInt32(send, i, ExpectedAllGatherValue(options.rankSize, options.rank, i));
                }
                break;
            case CollectiveOp::BROADCAST:
                for (int64_t i = 0; i < count; ++i) {
                    const int32_t value = options.rank == 0 ?
                        ExpectedAllGatherValue(options.rankSize, 0, i) : -1;
                    StoreInt32(send, i, value);
                }
                break;
            case CollectiveOp::NOOP:
                break;
        }
        std::fill(recv.begin(), recv.end(), 0xff);
        return true;
    }

    switch (options.op) {
        case CollectiveOp::ALLGATHER:
        case CollectiveOp::ALLREDUCE:
        case CollectiveOp::REDUCESCATTER:
            for (size_t i = 0; i < send.size(); ++i) {
                send[i] = PatternByte(options.rank, 0, static_cast<int64_t>(i));
            }
            break;
        case CollectiveOp::ALLTOALL: {
            const size_t bytesPerPeer = static_cast<size_t>(count) * options.dtype.bytes;
            for (int dst = 0; dst < options.rankSize; ++dst) {
                for (size_t i = 0; i < bytesPerPeer; ++i) {
                    send[static_cast<size_t>(dst) * bytesPerPeer + i] =
                        PatternByte(options.rank, dst, static_cast<int64_t>(i));
                }
            }
            break;
        }
        case CollectiveOp::BROADCAST:
            for (size_t i = 0; i < send.size(); ++i) {
                send[i] = options.rank == 0 ? PatternByte(0, 0, static_cast<int64_t>(i)) : 0xff;
            }
            break;
        case CollectiveOp::NOOP:
            break;
    }
    std::fill(recv.begin(), recv.end(), 0xff);
    return true;
}

int ValidateInt32(const Options &options, int64_t count, const std::vector<uint8_t> &recv)
{
    int errors = 0;
    switch (options.op) {
        case CollectiveOp::ALLGATHER:
            for (int src = 0; src < options.rankSize; ++src) {
                for (int64_t i = 0; i < count; ++i) {
                    const int64_t index = static_cast<int64_t>(src) * count + i;
                    const int32_t expected = ExpectedAllGatherValue(options.rankSize, src, i);
                    const int32_t actual = LoadInt32(recv, index);
                    if (actual != expected) {
                        if (errors < 8) {
                            std::cerr << "[rank " << options.rank << "] allgather mismatch src=" << src
                                      << " i=" << i << " expected=" << expected
                                      << " actual=" << actual << std::endl;
                        }
                        ++errors;
                    }
                }
            }
            break;
        case CollectiveOp::ALLTOALL:
            for (int src = 0; src < options.rankSize; ++src) {
                for (int64_t i = 0; i < count; ++i) {
                    const int64_t index = static_cast<int64_t>(src) * count + i;
                    const int32_t expected = ExpectedAllToAllValue(options.rankSize, src, options.rank, i);
                    const int32_t actual = LoadInt32(recv, index);
                    if (actual != expected) {
                        if (errors < 8) {
                            std::cerr << "[rank " << options.rank << "] alltoall mismatch src=" << src
                                      << " i=" << i << " expected=" << expected
                                      << " actual=" << actual << std::endl;
                        }
                        ++errors;
                    }
                }
            }
            break;
        case CollectiveOp::ALLREDUCE:
            for (int64_t i = 0; i < count; ++i) {
                const int32_t expected = ExpectedAllReduceSum(options.rankSize, i);
                const int32_t actual = LoadInt32(recv, i);
                if (actual != expected) {
                    if (errors < 8) {
                        std::cerr << "[rank " << options.rank << "] allreduce mismatch"
                                  << " i=" << i << " expected=" << expected
                                  << " actual=" << actual << std::endl;
                    }
                    ++errors;
                }
            }
            break;
        case CollectiveOp::REDUCESCATTER:
            for (int64_t i = 0; i < count; ++i) {
                const int32_t expected = ExpectedReduceScatterSum(options.rankSize, options.rank, count, i);
                const int32_t actual = LoadInt32(recv, i);
                if (actual != expected) {
                    if (errors < 8) {
                        std::cerr << "[rank " << options.rank << "] reducescatter mismatch"
                                  << " i=" << i << " expected=" << expected
                                  << " actual=" << actual << std::endl;
                    }
                    ++errors;
                }
            }
            break;
        case CollectiveOp::BROADCAST:
            for (int64_t i = 0; i < count; ++i) {
                const int32_t expected = ExpectedAllGatherValue(options.rankSize, 0, i);
                const int32_t actual = LoadInt32(recv, i);
                if (actual != expected) {
                    if (errors < 8) {
                        std::cerr << "[rank " << options.rank << "] broadcast mismatch"
                                  << " i=" << i << " expected=" << expected
                                  << " actual=" << actual << std::endl;
                    }
                    ++errors;
                }
            }
            break;
        case CollectiveOp::NOOP:
            break;
    }
    return errors;
}

int ValidateBytePattern(const Options &options, int64_t count, const std::vector<uint8_t> &recv)
{
    int errors = 0;
    const size_t bytesPerPeer = static_cast<size_t>(count) * options.dtype.bytes;
    switch (options.op) {
        case CollectiveOp::ALLGATHER:
            for (int src = 0; src < options.rankSize; ++src) {
                for (size_t i = 0; i < bytesPerPeer; ++i) {
                    const uint8_t expected = PatternByte(src, 0, static_cast<int64_t>(i));
                    const uint8_t actual = recv[static_cast<size_t>(src) * bytesPerPeer + i];
                    if (actual != expected) {
                        if (errors < 8) {
                            std::cerr << "[rank " << options.rank << "] byte allgather mismatch src=" << src
                                      << " byte=" << i << " expected=" << static_cast<int>(expected)
                                      << " actual=" << static_cast<int>(actual) << std::endl;
                        }
                        ++errors;
                    }
                }
            }
            break;
        case CollectiveOp::ALLTOALL:
            for (int src = 0; src < options.rankSize; ++src) {
                for (size_t i = 0; i < bytesPerPeer; ++i) {
                    const uint8_t expected = PatternByte(src, options.rank, static_cast<int64_t>(i));
                    const uint8_t actual = recv[static_cast<size_t>(src) * bytesPerPeer + i];
                    if (actual != expected) {
                        if (errors < 8) {
                            std::cerr << "[rank " << options.rank << "] byte alltoall mismatch src=" << src
                                      << " byte=" << i << " expected=" << static_cast<int>(expected)
                                      << " actual=" << static_cast<int>(actual) << std::endl;
                        }
                        ++errors;
                    }
                }
            }
            break;
        case CollectiveOp::BROADCAST:
            for (size_t i = 0; i < bytesPerPeer; ++i) {
                const uint8_t expected = PatternByte(0, 0, static_cast<int64_t>(i));
                const uint8_t actual = recv[i];
                if (actual != expected) {
                    if (errors < 8) {
                        std::cerr << "[rank " << options.rank << "] byte broadcast mismatch"
                                  << " byte=" << i << " expected=" << static_cast<int>(expected)
                                  << " actual=" << static_cast<int>(actual) << std::endl;
                    }
                    ++errors;
                }
            }
            break;
        case CollectiveOp::ALLREDUCE:
        case CollectiveOp::REDUCESCATTER:
            std::cerr << "[rank " << options.rank
                      << "] ERROR: byte validation is unsupported for allreduce/reducescatter" << std::endl;
            ++errors;
            break;
        case CollectiveOp::NOOP:
            break;
    }
    return errors;
}

double ComputeAlgBandwidthGbps(int64_t outputBytesPerRank, double avgUs)
{
    if (avgUs <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(outputBytesPerRank) / avgUs / 1000.0;
}

double ComputeBusBandwidthGbps(CollectiveOp op, int rankSize, double algBwGbps)
{
    if (op == CollectiveOp::NOOP) {
        return 0.0;
    }
    if (rankSize <= 0) {
        return 0.0;
    }
    return algBwGbps * static_cast<double>(rankSize - 1) / static_cast<double>(rankSize);
}

bool MeasureOnce(const Options &options, void *devSend, void *devRecv, int64_t count, TileXRCommPtr comm,
                 aclrtStream stream, double &us)
{
    aclrtEvent start = nullptr;
    aclrtEvent stop = nullptr;
    if (!CheckAcl(options.rank, "aclrtCreateEvent start", aclrtCreateEvent(&start)) ||
        !CheckAcl(options.rank, "aclrtCreateEvent stop", aclrtCreateEvent(&stop)) ||
        !CheckAcl(options.rank, "aclrtRecordEvent start", aclrtRecordEvent(start, stream))) {
        if (start != nullptr) {
            aclrtDestroyEvent(start);
        }
        if (stop != nullptr) {
            aclrtDestroyEvent(stop);
        }
        return false;
    }
    if (!CallCollective(options, devSend, devRecv, count, comm, stream)) {
        aclrtDestroyEvent(start);
        aclrtDestroyEvent(stop);
        return false;
    } else if (!CheckAcl(options.rank, "aclrtRecordEvent stop", aclrtRecordEvent(stop, stream))) {
        aclrtDestroyEvent(start);
        aclrtDestroyEvent(stop);
        return false;
    } else if (!CheckAcl(options.rank, "aclrtSynchronizeEvent stop", aclrtSynchronizeEvent(stop))) {
        aclrtDestroyEvent(start);
        aclrtDestroyEvent(stop);
        return false;
    }

    float elapsedMs = 0.0f;
    const bool ok = CheckAcl(options.rank, "aclrtEventElapsedTime",
        aclrtEventElapsedTime(&elapsedMs, start, stop));
    aclrtDestroyEvent(start);
    aclrtDestroyEvent(stop);
    us = static_cast<double>(elapsedMs) * 1000.0;
    if (!ok) {
        return false;
    }
    return true;
}

bool MeasureBatch(const Options &options, void *devSend, void *devRecv, int64_t count,
                  TileXRCommPtr comm, aclrtStream stream, Measurement &measurement)
{
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < options.iters; ++i) {
        if (!CallCollective(options, devSend, devRecv, count, comm, stream)) {
            return false;
        }
    }
    if (!CheckAcl(options.rank, "aclrtSynchronizeStream batch measurement",
            aclrtSynchronizeStream(stream))) {
        return false;
    }
    const auto stop = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::micro> elapsed = stop - start;
    measurement.avgUs = elapsed.count() / static_cast<double>(options.iters);
    measurement.hasLatencyRange = false;
    return true;
}

bool Measure(const Options &options, void *devSend, void *devRecv, int64_t count, TileXRCommPtr comm,
             aclrtStream stream, Measurement &measurement)
{
    for (int i = 0; i < options.warmupIters; ++i) {
        if (!CallCollective(options, devSend, devRecv, count, comm, stream)) {
            return false;
        }
    }
    if (!CheckAcl(options.rank, "aclrtSynchronizeStream warmup", aclrtSynchronizeStream(stream))) {
        return false;
    }

    if (options.batchTiming) {
        return MeasureBatch(options, devSend, devRecv, count, comm, stream, measurement);
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(options.iters));
    for (int i = 0; i < options.iters; ++i) {
        double us = 0.0;
        if (!MeasureOnce(options, devSend, devRecv, count, comm, stream, us)) {
            return false;
        }
        samples.push_back(us);
    }
    const auto minmax = std::minmax_element(samples.begin(), samples.end());
    double sum = 0.0;
    for (double value : samples) {
        sum += value;
    }
    measurement.avgUs = sum / static_cast<double>(samples.size());
    measurement.minUs = *minmax.first;
    measurement.maxUs = *minmax.second;
    return true;
}

void PrintHeader()
{
    std::cout << "op dtype ranks bytes count iters algbw(GB/s) busbw(GB/s) avg(us) min(us) max(us) errors"
              << std::endl;
}

void PrintRow(const Row &row)
{
    std::cout << row.op << ' ' << row.dtype << ' ' << row.ranks << ' ' << row.bytes << ' '
              << row.count << ' ' << row.iters << ' '
              << std::fixed << std::setprecision(3)
              << row.algBw << ' ' << row.busBw << ' ' << row.avgUs << ' ';
    if (row.hasLatencyRange) {
        std::cout << row.minUs << ' ' << row.maxUs;
    } else {
        std::cout << "N/A N/A";
    }
    std::cout << ' ' << row.errors << std::endl;
}

bool AppendCsv(const std::string &path, const Row &row)
{
    if (path.empty()) {
        return true;
    }
    std::ifstream existing(path.c_str());
    const bool needsHeader = !existing.good() || existing.peek() == std::ifstream::traits_type::eof();
    existing.close();

    std::ofstream out(path.c_str(), std::ios::app);
    if (!out.is_open()) {
        std::cerr << "ERROR: failed to open CSV " << path << std::endl;
        return false;
    }
    if (needsHeader) {
        out << "op,dtype,ranks,bytes,count,iters,algbw(GB/s),busbw(GB/s),avg(us),min(us),max(us),errors\n";
    }
    out << row.op << ',' << row.dtype << ',' << row.ranks << ',' << row.bytes << ','
        << row.count << ',' << row.iters << ',' << std::fixed << std::setprecision(6)
        << row.algBw << ',' << row.busBw << ',' << row.avgUs << ',';
    if (row.hasLatencyRange) {
        out << row.minUs << ',' << row.maxUs;
    } else {
        out << "N/A,N/A";
    }
    out << ',' << row.errors << '\n';
    return true;
}

bool Cleanup(int rank, TileXRCommPtr comm, aclrtStream stream, int deviceId, bool deviceSet)
{
    if (comm != nullptr) {
        const int ret = TileXRCommDestroy(comm);
        if (ret != TileXR::TILEXR_SUCCESS) {
            std::cerr << "[rank " << rank
                      << "] ERROR: TileXRCommDestroy failed, ret="
                      << ret << std::endl;
            return false;
        }
    }
    bool ok = true;
    if (stream != nullptr) {
        ok = CheckAcl(rank, "aclrtDestroyStream", aclrtDestroyStream(stream)) && ok;
    }
    if (deviceSet) {
        ok = CheckAcl(rank, "aclrtResetDevice", aclrtResetDevice(deviceId)) && ok;
    }
    ok = CheckAcl(rank, "aclFinalize", aclFinalize()) && ok;
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage(argv[0]);
        return 2;
    }

    const int deviceId = options.deviceIdOverride >= 0 ? options.deviceIdOverride : options.firstNpu + options.rank;
    TileXRCommPtr comm = nullptr;
    aclrtStream stream = nullptr;
    bool deviceSet = false;
    if (!CheckAcl(options.rank, "aclInit", aclInit(nullptr))) {
        return 1;
    }
    if (!CheckAcl(options.rank, "aclrtSetDevice", aclrtSetDevice(deviceId))) {
        Cleanup(options.rank, comm, stream, deviceId, deviceSet);
        return 1;
    }
    deviceSet = true;
    if (!CheckAcl(options.rank, "aclrtCreateStream", aclrtCreateStream(&stream))) {
        Cleanup(options.rank, comm, stream, deviceId, deviceSet);
        return 1;
    }

    bool commOk = false;
    if (options.commMode == CommMode::SOCKET) {
        TileXRUniqueId uniqueId {};
        commOk = CheckTileXR(options.rank, "TileXRGetUniqueId", TileXRGetUniqueId(&uniqueId, 0)) &&
            CheckTileXR(options.rank, "TileXRCommInitRank",
                TileXRCommInitRank(uniqueId, options.rankSize, options.rank, &comm));
    } else if (options.commMode == CommMode::SHARED_QP) {
        commOk = CheckTileXR(options.rank, "TileXRCommInitRankWithSharedQpDomain",
            TileXRCommInitRankWithSharedQpDomain(
                options.commDomain, options.rankSize, options.rank, &comm));
    } else {
        commOk = CheckTileXR(options.rank, "TileXRCommInitRankLocal",
            TileXRCommInitRankLocal(options.rankSize, options.rank, &comm));
    }
    if (!commOk) {
        Cleanup(options.rank, comm, stream, deviceId, deviceSet);
        return 1;
    }

    int totalErrors = 0;
    if (options.rank == 0) {
        PrintHeader();
    }

    for (int64_t bytes = options.minBytes;;) {
        int64_t count = bytes / MessageSizeDivisor(options);
        if (count <= 0) {
            count = 1;
        }
        int64_t sendElements = 0;
        int64_t recvElements = 0;
        int64_t sendBytes = 0;
        int64_t recvBytes = 0;
        if (!ComputeMessageSizes(options, count, sendElements, recvElements, sendBytes, recvBytes)) {
            std::cerr << "ERROR: message size too large" << std::endl;
            Cleanup(options.rank, comm, stream, deviceId, deviceSet);
            return 1;
        }
        const int64_t actualSendBytesPerRank = sendBytes;

        std::vector<uint8_t> hostSend(static_cast<size_t>(sendBytes));
        std::vector<uint8_t> hostRecv(static_cast<size_t>(recvBytes));
        FillPattern(options, count, hostSend, hostRecv);

        void *devSend = nullptr;
        void *devRecv = nullptr;
        bool ok = CheckAcl(options.rank, "aclrtMalloc send",
            aclrtMalloc(&devSend, static_cast<size_t>(sendBytes), ACL_MEM_MALLOC_HUGE_FIRST)) &&
            CheckAcl(options.rank, "aclrtMemcpy H2D send",
                aclrtMemcpy(devSend, static_cast<size_t>(sendBytes), hostSend.data(),
                    static_cast<size_t>(sendBytes), ACL_MEMCPY_HOST_TO_DEVICE));
        if (ok) {
            if (options.op == CollectiveOp::BROADCAST || options.op == CollectiveOp::NOOP) {
                devRecv = devSend;
            } else {
                ok = CheckAcl(options.rank, "aclrtMalloc recv",
                        aclrtMalloc(&devRecv, static_cast<size_t>(recvBytes), ACL_MEM_MALLOC_HUGE_FIRST)) &&
                    CheckAcl(options.rank, "aclrtMemcpy H2D recv",
                        aclrtMemcpy(devRecv, static_cast<size_t>(recvBytes), hostRecv.data(),
                            static_cast<size_t>(recvBytes), ACL_MEMCPY_HOST_TO_DEVICE));
            }
        }

        Measurement measurement;
        if (ok) {
            ok = Measure(options, devSend, devRecv, count, comm, stream, measurement);
        }

        int errors = 0;
        if (ok && options.check && options.op != CollectiveOp::NOOP) {
            if (options.op != CollectiveOp::BROADCAST) {
                std::fill(hostRecv.begin(), hostRecv.end(), 0xff);
                ok = CheckAcl(options.rank, "aclrtMemcpy H2D devRecv sentinel",
                    aclrtMemcpy(devRecv, static_cast<size_t>(recvBytes), hostRecv.data(),
                        static_cast<size_t>(recvBytes), ACL_MEMCPY_HOST_TO_DEVICE));
            } else {
                ok = CheckAcl(options.rank, "aclrtMemcpy H2D broadcast check input",
                    aclrtMemcpy(devSend, static_cast<size_t>(sendBytes), hostSend.data(),
                        static_cast<size_t>(sendBytes), ACL_MEMCPY_HOST_TO_DEVICE));
            }
            ok = ok &&
                CallCollective(options, devSend, devRecv, count, comm, stream) &&
                CheckAcl(options.rank, "aclrtSynchronizeStream check", aclrtSynchronizeStream(stream)) &&
                CheckAcl(options.rank, "aclrtMemcpy D2H recv",
                    aclrtMemcpy(hostRecv.data(), static_cast<size_t>(recvBytes), devRecv,
                        static_cast<size_t>(recvBytes), ACL_MEMCPY_DEVICE_TO_HOST));
            if (ok) {
                errors = options.dtype.name == "int32" ?
                    ValidateInt32(options, count, hostRecv) : ValidateBytePattern(options, count, hostRecv);
            } else {
                errors = 1;
            }
        }

        if (devSend != nullptr) {
            aclrtFree(devSend);
        }
        if (devRecv != nullptr && devRecv != devSend) {
            aclrtFree(devRecv);
        }
        if (!ok) {
            Cleanup(options.rank, comm, stream, deviceId, deviceSet);
            return 1;
        }

        const std::string opName = OpName(options.op);
        const int64_t outputBytesPerRank = recvBytes;
        const double algBw = ComputeAlgBandwidthGbps(outputBytesPerRank, measurement.avgUs);
        const double busBw = ComputeBusBandwidthGbps(options.op, options.rankSize, algBw);
        if (options.minAlgBw >= 0.0 && algBw < options.minAlgBw) {
            ++errors;
        }
        if (options.maxLatencyUs >= 0.0 && measurement.avgUs > options.maxLatencyUs) {
            ++errors;
        }
        totalErrors += errors;

        Row row{opName, options.dtype.name, options.rankSize, actualSendBytesPerRank, count, options.iters,
            algBw, busBw, measurement.avgUs, measurement.minUs, measurement.maxUs,
            measurement.hasLatencyRange, errors};
        if (options.rank == 0) {
            PrintRow(row);
            if (!AppendCsv(options.csvPath, row)) {
                Cleanup(options.rank, comm, stream, deviceId, deviceSet);
                return 1;
            }
        }

        int64_t nextBytes = 0;
        if (!AdvanceBytes(bytes, options.stepFactor, options.maxBytes, nextBytes)) {
            std::cerr << "ERROR: message size step overflow" << std::endl;
            Cleanup(options.rank, comm, stream, deviceId, deviceSet);
            return 1;
        }
        if (nextBytes <= bytes) {
            break;
        }
        bytes = nextBytes;
    }

    const bool cleanupOk = Cleanup(
        options.rank, comm, stream, deviceId, deviceSet);
    return totalErrors == 0 && cleanupOk ? 0 : 1;
}
