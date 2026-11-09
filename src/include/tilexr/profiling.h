/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
// 功能：按 Host/device 编译环境导出 profiling 公共接口，供算子和捕获工具使用。
// Purpose: Expose profiling APIs for Host/device compilation to operators and capture tools.
#ifndef TILEXR_PUBLIC_PROFILING_H
#define TILEXR_PUBLIC_PROFILING_H
#include "profiling/types.h"
#if defined(__CCE__) && defined(__CCE_IS_AICORE__)
#include "profiling/device.h"
#include "profiling/direct.h"
#else
#include "profiling/host.h"
#include "profiling/file.h"
#include "profiling/events.h"
#include "profiling/workspace.h"
// Pure Host layout/encoding consumers need no CANN installation.
#if defined(__has_include)
#if __has_include(<acl/acl.h>)
#include "profiling/acl.h"
#include "profiling/manifest.h"
#endif
#elif defined(TILEXR_PROFILING_WITH_ACL) && TILEXR_PROFILING_WITH_ACL
#include "profiling/acl.h"
#endif
#endif
#endif
