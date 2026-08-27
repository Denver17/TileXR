#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#ifdef TILEXR_SOURCE_ROOT
const char *kSourceRoot = TILEXR_SOURCE_ROOT;
#else
const char *kSourceRoot = ".";
#endif

std::string JoinPath(const std::string &base, const std::string &path)
{
    if (base.empty() || base[base.size() - 1] == '/') {
        return base + path;
    }
    return base + "/" + path;
}

bool ReadFile(const std::string &relativePath, std::string *contents)
{
    const std::string fullPath = JoinPath(kSourceRoot, relativePath);
    std::ifstream stream(fullPath.c_str());
    if (!stream.is_open()) {
        std::cerr << "missing file: " << relativePath << std::endl;
        ++g_failures;
        return false;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    *contents = buffer.str();
    return true;
}

void CheckContains(const std::string &label, const std::string &contents, const std::string &needle)
{
    if (contents.find(needle) == std::string::npos) {
        std::cerr << label << " missing: " << needle << std::endl;
        ++g_failures;
    }
}

void CheckNotContains(const std::string &label, const std::string &contents, const std::string &needle)
{
    if (contents.find(needle) != std::string::npos) {
        std::cerr << label << " contains forbidden string: " << needle << std::endl;
        ++g_failures;
    }
}

void TestPublicHeader()
{
    std::string contents;
    if (!ReadFile("src/include/tilexr_ep.h", &contents)) {
        return;
    }

    CheckContains("src/include/tilexr_ep.h", contents, "#ifdef __cplusplus");
    CheckContains("src/include/tilexr_ep.h", contents, "extern \"C\"");
    CheckContains("src/include/tilexr_ep.h", contents, "int TileXRMoeEpDispatch(");
    CheckContains("src/include/tilexr_ep.h", contents, "int TileXRMoeEpDispatchV2(");
    CheckContains("src/include/tilexr_ep.h", contents, "int TileXRMoeEpCombine(");
    CheckContains("src/include/tilexr_ep.h", contents, "int TileXRMoeEpCombineV2(");
    CheckContains("src/include/tilexr_ep.h", contents, "TileXRCommPtr comm");
    CheckContains("src/include/tilexr_ep.h", contents, "TileXR::TileXRDataType dtype");
    CheckContains("src/include/tilexr_ep.h", contents, "aclrtStream stream");
    CheckContains("src/include/tilexr_ep.h", contents, "xActiveMask");
    CheckContains("src/include/tilexr_ep.h", contents, "dynamicScalesOut");
    CheckContains("src/include/tilexr_ep.h", contents, "tpRecvCountsOut");
    CheckContains("src/include/tilexr_ep.h", contents, "expandScalesOut");
    CheckContains("src/include/tilexr_ep.h", contents, "workspace");
    CheckContains("src/include/tilexr_ep.h", contents, "quantMode");
    CheckContains("src/include/tilexr_ep.h", contents, "tpWorldSize");
    CheckContains("src/include/tilexr_ep.h", contents, "sharedExpertNum");

    std::string apiContents;
    if (ReadFile("src/include/tilexr_api.h", &apiContents)) {
        CheckContains("src/include/tilexr_api.h", apiContents, "TileXRGetUDMARegistryHost");
    }
}

void TestBuildPlacement()
{
    std::string rootCmake;
    if (ReadFile("CMakeLists.txt", &rootCmake)) {
        CheckContains("CMakeLists.txt", rootCmake,
            "option(TILEXR_BUILD_EP \"Build TileXR EP communication library\" OFF)");
        CheckContains("CMakeLists.txt", rootCmake, "add_subdirectory(src/ep)");
    }

    std::string epCmake;
    if (ReadFile("src/ep/CMakeLists.txt", &epCmake)) {
        CheckContains("src/ep/CMakeLists.txt", epCmake, "add_library(tilexr-ep SHARED");
        CheckContains("src/ep/CMakeLists.txt", epCmake, "tile-comm");
        CheckContains("src/ep/CMakeLists.txt", epCmake, "libtilexr_ep_combine_kernel.so");
        CheckContains("src/ep/CMakeLists.txt", epCmake, "tilexr_ep.h");
        CheckContains("src/ep/CMakeLists.txt", epCmake, "install(TARGETS tilexr-ep");
    }
}

void TestEpHostChecksRegisteredWorkspace()
{
    std::string launchContext;
    if (!ReadFile("src/ep/host/ep_launch_context.cpp", &launchContext)) {
        return;
    }

    CheckContains("src/ep/host/ep_launch_context.cpp", launchContext, "ValidateRegisteredWorkspace");
    CheckContains("src/ep/host/ep_launch_context.cpp", launchContext, "TileXRGetUDMARegistryHost");
    CheckContains("src/ep/host/ep_launch_context.cpp", launchContext, "UDMARegionContains");
    CheckContains("src/ep/host/ep_launch_context.cpp", launchContext, "TileXREpUdmaRequiredWorkspaceBytes");
}

void TestEpSocDefaultFollowsEnvironment()
{
    std::string epCmake;
    if (!ReadFile("src/ep/CMakeLists.txt", &epCmake)) {
        return;
    }

    CheckContains("src/ep/CMakeLists.txt", epCmake, "$ENV{TILEXR_SOC_NAME}");
    CheckContains("src/ep/CMakeLists.txt", epCmake, "string(TOLOWER");
    CheckContains("src/ep/CMakeLists.txt", epCmake, "ascend910b");
    CheckContains("src/ep/CMakeLists.txt", epCmake, "dav-c220-vec");
    CheckContains("src/ep/CMakeLists.txt", epCmake, "Ascend910B");
}

void TestChipMapRecognizesAscend950Dt9582()
{
    std::string internal;
    if (!ReadFile("src/comm/tilexr_internal.cpp", &internal)) {
        return;
    }

    CheckContains("src/comm/tilexr_internal.cpp", internal, "\"Ascend950DT_9582\", ChipName::CHIP_950");
    CheckContains("src/comm/tilexr_internal.cpp", internal, "\"Ascend950PR_9599\", ChipName::CHIP_950PR");
    CheckContains("src/comm/tilexr_internal.cpp", internal, "bool UseLegacyIpcPid(ChipName chipName)");
    CheckContains("src/comm/tilexr_internal.cpp", internal, "chipName == ChipName::CHIP_950PR");
}

void TestEpKernelUsesCceArchFlags()
{
    std::string epCmake;
    if (!ReadFile("src/ep/CMakeLists.txt", &epCmake)) {
        return;
    }

    CheckContains("src/ep/CMakeLists.txt", epCmake, "-xcce");
    CheckContains("src/ep/CMakeLists.txt", epCmake, "${TILEXR_EP_AICORE_ARCH}");
    CheckContains("src/ep/CMakeLists.txt", epCmake, "set(TILEXR_EP_KERNEL_LINK_OPTIONS ${TILEXR_EP_AICORE_ARCH})");
    CheckNotContains("src/ep/CMakeLists.txt", epCmake, "-xasc");
    CheckNotContains("src/ep/CMakeLists.txt", epCmake, "--npu-arch=");
    CheckNotContains("src/ep/CMakeLists.txt", epCmake, "--cce-auto-infer-kernel-type=false");
    CheckNotContains("src/ep/CMakeLists.txt", epCmake, "--cce-fatobj-link");
}

void TestSyncFlagBuffersUseFullFlagUnit()
{
    std::string syncHeader;
    if (!ReadFile("src/include/tilexr_sync.h", &syncHeader)) {
        return;
    }

    CheckContains("src/include/tilexr_sync.h", syncHeader,
        "LocalTensor<int64_t> localSet = tBuf.GetWithOffset<int64_t>(FLAG_UNIT_INT_NUM, 0);");
    CheckContains("src/include/tilexr_sync.h", syncHeader,
        "LocalTensor<int64_t> localWait = tBuf.GetWithOffset<int64_t>(FLAG_UNIT_INT_NUM, 0);");
}

const char *kRemoteDeployScript = "tests/ep/demo/deploy_and_run_remote.sh";

void TestRemoteDeployScriptCleansRemoteCheckout()
{
    std::string deployScript;
    if (!ReadFile(kRemoteDeployScript, &deployScript)) {
        return;
    }

    CheckContains(kRemoteDeployScript, deployScript, "case \"\\${remote_repo}\" in");
    CheckContains(kRemoteDeployScript, deployScript, "Refusing to clean unexpected remote repo");
    CheckContains(kRemoteDeployScript, deployScript, "rm -rf -- \"\\${remote_repo}\"");
    CheckContains(kRemoteDeployScript, deployScript, "mkdir -p -- \"\\${remote_repo}\"");
}

void TestRemoteDeployScriptDoesNotInitializeSubmodules()
{
    std::string deployScript;
    if (!ReadFile(kRemoteDeployScript, &deployScript)) {
        return;
    }

    CheckNotContains(kRemoteDeployScript, deployScript, "submodule update");
    CheckNotContains(kRemoteDeployScript, deployScript, "3rdparty/hcomm");
    CheckNotContains(kRemoteDeployScript, deployScript, "ops-transformer");
    CheckNotContains(kRemoteDeployScript, deployScript, "3rdparty/shmem");
    CheckNotContains(kRemoteDeployScript, deployScript, "3rdparty/spdlog");
}

void TestRemoteDeployScriptDoesNotExposePrivateRemoteDefaults()
{
    std::string deployScript;
    if (!ReadFile(kRemoteDeployScript, &deployScript)) {
        return;
    }

    CheckContains(kRemoteDeployScript, deployScript, "TILEXR_EP_REMOTE:?set TILEXR_EP_REMOTE");
    CheckContains(kRemoteDeployScript, deployScript, "TILEXR_EP_REMOTE_BASE:?set TILEXR_EP_REMOTE_BASE");
    CheckNotContains(kRemoteDeployScript, deployScript, "TILEXR_EP_REMOTE:-");
    CheckNotContains(kRemoteDeployScript, deployScript, "TILEXR_EP_REMOTE_BASE:-");
    CheckNotContains(kRemoteDeployScript, deployScript, "REMOTE_BASE=/");
}

void TestDemoRunnerUsesLibAndLib64Paths()
{
    std::string runner;
    if (!ReadFile("tests/ep/demo/run_tilexr_ep_dispatch_demo.sh", &runner)) {
        return;
    }

    CheckContains("tests/ep/demo/run_tilexr_ep_dispatch_demo.sh", runner, "${TILEXR_ROOT}/install/lib64");
    CheckContains("tests/ep/demo/run_tilexr_ep_dispatch_demo.sh", runner, "${TILEXR_ROOT}/install/lib");
    CheckContains("tests/ep/demo/run_tilexr_ep_dispatch_demo.sh", runner, "${INSTALL_DIR}/lib64");
    CheckContains("tests/ep/demo/run_tilexr_ep_dispatch_demo.sh", runner, "${INSTALL_DIR}/lib");
}

void TestDispatchDemoDoesNotEmbedRuntimeSearchPaths()
{
    std::string epCmake;
    if (ReadFile("tests/ep/CMakeLists.txt", &epCmake)) {
        CheckContains("tests/ep/CMakeLists.txt", epCmake,
            "set_target_properties(tilexr_ep_dispatch_demo PROPERTIES");
        CheckContains("tests/ep/CMakeLists.txt", epCmake, "SKIP_BUILD_RPATH TRUE");
        CheckContains("tests/ep/CMakeLists.txt", epCmake, "INSTALL_RPATH \"\"");
    }
}

void TestDemoRunnerSelectsBackendForSoc()
{
    std::string runner;
    if (!ReadFile("tests/ep/demo/run_tilexr_ep_dispatch_demo.sh", &runner)) {
        return;
    }

    CheckContains("tests/ep/demo/run_tilexr_ep_dispatch_demo.sh", runner,
        "ascend950*) default_impl=\"udma\" ;;");
    CheckContains("tests/ep/demo/run_tilexr_ep_dispatch_demo.sh", runner,
        "*) default_impl=\"memory\" ;;");
    CheckContains("tests/ep/demo/run_tilexr_ep_dispatch_demo.sh", runner,
        "impl=\"${5:-${TILEXR_EP_DEMO_IMPL:-${default_impl}}}\"");
}

void TestDispatchDemoRegistersAlignedUdmaWorkspace()
{
    std::string demo;
    if (!ReadFile("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", &demo)) {
        return;
    }

    CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "AlignAddress");
    CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "rawWorkspaceDev");
    CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo,
        "workspaceDev = reinterpret_cast<void *>(AlignAddress(");
    CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo,
        "TileXRUDMARegister(comm, static_cast<GM_ADDR>(workspaceDev), workspaceBytes");
    CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "EpRequiredWorkspaceBytes");
}

void TestDispatchDemoUsesHostBarrierBeforeValidation()
{
    std::string demo;
    if (!ReadFile("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", &demo)) {
        return;
    }

    CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "DemoBarrierAll");
    CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "TILEXR_DEMO_BARRIER_ADDR");
    CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "dispatch synchronized");
}

void TestNoForbiddenDependencies()
{
    const std::vector<std::string> paths = {
        "src/include/tilexr_ep.h",
        "src/ep/CMakeLists.txt",
        "src/ep/host/ep_layout.h",
        "src/ep/host/ep_layout.cpp",
    };
    const std::vector<std::string> forbidden = {
        "ops-transformer",
        "GetHcclContext",
        "TileXRUDMARegister",
        "UDMAPut",
        "shmem",
    };

    for (std::vector<std::string>::const_iterator path = paths.begin(); path != paths.end(); ++path) {
        std::string contents;
        if (!ReadFile(*path, &contents)) {
            continue;
        }
        for (std::vector<std::string>::const_iterator needle = forbidden.begin(); needle != forbidden.end(); ++needle) {
            CheckNotContains(*path, contents, *needle);
        }
    }
}

} // namespace

int main()
{
    TestPublicHeader();
    TestBuildPlacement();
    TestEpHostChecksRegisteredWorkspace();
    TestEpSocDefaultFollowsEnvironment();
    TestChipMapRecognizesAscend950Dt9582();
    TestEpKernelUsesCceArchFlags();
    TestSyncFlagBuffersUseFullFlagUnit();
    TestRemoteDeployScriptCleansRemoteCheckout();
    TestRemoteDeployScriptDoesNotInitializeSubmodules();
    TestRemoteDeployScriptDoesNotExposePrivateRemoteDefaults();
    TestDemoRunnerUsesLibAndLib64Paths();
    TestDispatchDemoDoesNotEmbedRuntimeSearchPaths();
    TestDemoRunnerSelectsBackendForSoc();
    TestDispatchDemoRegistersAlignedUdmaWorkspace();
    TestDispatchDemoUsesHostBarrierBeforeValidation();
    TestNoForbiddenDependencies();
    return g_failures == 0 ? 0 : 1;
}
