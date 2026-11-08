/* Copyright (c) 2026 TileXR Project. Licensed under the Apache License, Version 2.0. */

/*
 * 功能：确定 RDMA Debug 参数检查和 AICore profiling 的编译开关；未显式指定 Debug 时按 NDEBUG 选择默认值。
 * 调用关系：WRITE trace 引入本文件，device 和 pipeline 代码使用这些宏选择检查与记录路径。
 *
 * Purpose: Define RDMA Debug-validation and AICore-profiling switches, deriving the default Debug value from NDEBUG.
 * Calls: WRITE trace includes this header; device and pipeline code use its macros to select validation and recording paths.
 */
#ifndef TILEXR_RDMA_DEBUG_H
#define TILEXR_RDMA_DEBUG_H
#ifndef TILEXR_RDMA_DEBUG
#ifdef NDEBUG
#define TILEXR_RDMA_DEBUG 0
#else
#define TILEXR_RDMA_DEBUG 1
#endif
#endif
#if TILEXR_RDMA_DEBUG != 0 && TILEXR_RDMA_DEBUG != 1
#error TILEXR_RDMA_DEBUG must be 0 or 1
#endif
#if TILEXR_RDMA_DEBUG && defined(TILEXR_ENABLE_PROFILING) && TILEXR_ENABLE_PROFILING == 1 && \
    defined(__CCE__) && defined(__CCE_IS_AICORE__)
#define TILEXR_RDMA_PROFILE_ACTIVE 1
#else
#define TILEXR_RDMA_PROFILE_ACTIVE 0
#endif
#endif
