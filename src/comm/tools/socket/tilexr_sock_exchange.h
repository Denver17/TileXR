/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*
 * 功能：声明 bootstrap 类型和连接接口，实现经 rank 0 收集、分发数据的 AllGather，以及完整收发循环。
 * 调用关系：TileXRComm 和 RDMA 初始化使用此控制通道；连接建立和关闭由 tilexr_sock_exchange.cpp 实现。
 * 调用约定：send/receive buffer 由调用方提供；各 rank 须按相同顺序进入 collective。
 *
 * Purpose: Declare bootstrap and connection interfaces, implement AllGather through rank 0 and handle partial socket I/O.
 * Calls: TileXRComm and RDMA setup use this control channel; tilexr_sock_exchange.cpp establishes and closes connections.
 * Contract: Callers provide send/receive buffers; ranks must enter collectives in the same order.
 */
#ifndef TILEXR_SOCK_EXCHANGE_H
#define TILEXR_SOCK_EXCHANGE_H

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <memory>

#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "tilexr_log.h"

#include "tilexr_types.h"
#include "tilexr_api.h"

namespace TileXR {
/* Common socket address storage structure for IPv4/IPv6 */
union TileXRSocketAddress {
    struct sockaddr sa;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
};

constexpr uint64_t TILEXR_MAGIC = 0xdddd0000dddd0000;

struct TileXRBootstrapHandle {
    uint64_t magic;
    union TileXRSocketAddress addr;
};
union TileXRBootstrap {
    TileXRBootstrapHandle handle;
    TileXRUniqueId uid;
};

int BootstrapGetUniqueId(TileXRBootstrapHandle *handle, int commDomain);

class TileXRSockExchange {
public:
    TileXRSockExchange(int rank, int rankSize, int commDomain);
    TileXRSockExchange(int rank, int rankSize, TileXRUniqueId tilexrCommId);
    ~TileXRSockExchange();

    /* *
     * @brief All gather data from @ref sendBuf to @ref recvBuf
     *
     * @note recvBuf's space must larger than sendSize * rankSize_
     * @return TILEXR_SUCCESS for success, other for failed
     */
    template <typename T> int AllGather(const T *sendBuf, size_t sendCount, T *recvBuf,
                                       uint32_t timeoutMs = 0)
    {
        if (!isInit_ && Prepare() != TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }
        isInit_ = true;
        if (ConfigureIoTimeout(timeoutMs) != TILEXR_SUCCESS) {
            return TILEXR_ERROR_INTERNAL;
        }

        if (!IsServer()) {
            return ClientSendRecv(sendBuf, sendCount, recvBuf);
        } else {
            return ServerRecvSend(sendBuf, sendCount, recvBuf);
        }
    }

    int GetNodeNum();

    static bool CheckValid(TileXRUniqueId tilexrCommId)
    {
        TileXRBootstrap id {};
        id.uid = tilexrCommId;
        return id.handle.magic == TILEXR_MAGIC;
    }

private:
    void GetIpAndPort();
    int Prepare();
    int Listen();
    int Accept();
    int ConfigureIoTimeout(uint32_t timeoutMs) const;
    void Close(int &fd) const;
    int Connect();
    int AcceptConnection(int fd, sockaddr_in &clientAddr, socklen_t *sinSize) const;
    void Cleanup();
    bool IsServer() const;
    static bool CheckErrno(int ioErrno)
    {
        return ioErrno == EINTR;
    }

    template <typename T> ssize_t Send(int fd, const T *sendBuf, size_t sendSize, int flag) const
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(sendBuf);
        size_t sent = 0;
        int sendFlags = flag;
#ifdef MSG_NOSIGNAL
        sendFlags |= MSG_NOSIGNAL;
#endif
        while (sent < sendSize) {
            const ssize_t ret = send(fd, bytes + sent, sendSize - sent, sendFlags);
            if (ret < 0) {
                if (CheckErrno(errno)) {
                    continue;
                }
                TILEXR_LOG(ERROR) << "Send failed after " << sent << "/" << sendSize
                                  << " bytes: " << strerror(errno);
                return -1;
            }
            if (ret == 0) {
                TILEXR_LOG(ERROR) << "Send returned zero after " << sent << "/" << sendSize << " bytes";
                return -1;
            }
            sent += static_cast<size_t>(ret);
        }
        return static_cast<ssize_t>(sent);
    }

    template <typename T> ssize_t Recv(int fd, T *recvBuf, size_t recvSize, int flag) const
    {
        auto* bytes = reinterpret_cast<uint8_t*>(recvBuf);
        size_t received = 0;
        while (received < recvSize) {
            const ssize_t ret = recv(fd, bytes + received, recvSize - received, flag);
            if (ret < 0) {
                if (CheckErrno(errno)) {
                    continue;
                }
                TILEXR_LOG(ERROR) << "Recv failed after " << received << "/" << recvSize
                                  << " bytes: " << strerror(errno);
                return -1;
            }
            if (ret == 0) {
                TILEXR_LOG(ERROR) << "Recv reached EOF after " << received << "/" << recvSize << " bytes";
                return -1;
            }
            received += static_cast<size_t>(ret);
        }
        return static_cast<ssize_t>(received);
    }

    template <typename T> int ClientSendRecv(const T *sendBuf, size_t sendSize, T *recvBuf)
    {
        if (Send(fd_, sendBuf, sendSize * sizeof(T), 0) <= 0) {
            TILEXR_LOG(ERROR) << "Client side " << rank_ << " send buffer failed";
            return TILEXR_ERROR_INTERNAL;
        }

        if (Recv(fd_, recvBuf, sendSize * rankSize_ * sizeof(T), MSG_WAITALL) <= 0) {
            TILEXR_LOG(ERROR) << "Client side " << rank_ << " recv buffer failed ";
            return TILEXR_ERROR_INTERNAL;
        }

        return TILEXR_SUCCESS;
    }

    template <typename T> int ServerRecvSend(const T *sendBuf, size_t sendSize, T *recvBuf)
    {
        for (size_t i = 0; i < sendSize; ++i) {
            recvBuf[i] = sendBuf[i];
        }

        for (int i = 1; i < rankSize_; ++i) {
            if (Recv(clientFds_[i], recvBuf + i * sendSize, sendSize * sizeof(T), MSG_WAITALL) <= 0) {
                TILEXR_LOG(ERROR) << "Server side recv rank " << i << " buffer failed";
                return TILEXR_ERROR_INTERNAL;
            }
        }

        for (int i = 1; i < rankSize_; ++i) {
            if (Send(clientFds_[i], recvBuf, sendSize * rankSize_ * sizeof(T), 0) <= 0) {
                TILEXR_LOG(ERROR) << "Server side send rank " << i << " buffer failed";
                return TILEXR_ERROR_INTERNAL;
            }
        }

        return TILEXR_SUCCESS;
    }
    int rank_ = 0;
    int rankSize_ = 0;
    int fd_ = -1;
    std::vector<int> clientFds_ = {};
    bool isInit_ = false;
    int commDomain_ = -1;
    std::string ip_;
    uint16_t port_ = 0;
    TileXRBootstrap tilexrCommId_ = {};
};
} // namespace TileXR

#endif
