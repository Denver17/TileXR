#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

std::string RepoPath(const std::string& path)
{
#ifdef TILEXR_SOURCE_ROOT
    return std::string(TILEXR_SOURCE_ROOT) + "/" + path;
#else
    return path;
#endif
}

std::string ReadFile(const std::string& path)
{
    std::ifstream input(RepoPath(path).c_str());
    if (!input.is_open()) {
        std::cerr << "failed to open " << RepoPath(path) << std::endl;
        ++g_failures;
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void CheckContains(const std::string& path, const std::string& text, const std::string& needle)
{
    if (text.find(needle) == std::string::npos) {
        std::cerr << "expected text not found in " << path << ": " << needle << std::endl;
        ++g_failures;
    }
}

void CheckNotContains(const std::string& path, const std::string& text, const std::string& needle)
{
    const auto pos = text.find(needle);
    if (pos != std::string::npos) {
        std::cerr << "unexpected text in " << path << ": " << needle
                  << " at byte " << pos << std::endl;
        ++g_failures;
    }
}

std::string RunCommand(const std::string& command)
{
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        std::cerr << "failed to run command: " << command << std::endl;
        ++g_failures;
        return {};
    }

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    const int status = pclose(pipe);
    if (status != 0) {
        std::cerr << "command failed: " << command << std::endl;
        ++g_failures;
    }
    return output;
}

void CheckNoLineContains(const std::string& description, const std::string& text, const std::string& needle)
{
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find(needle) != std::string::npos) {
            std::cerr << "unexpected " << description << ": " << line << std::endl;
            ++g_failures;
        }
    }
}

void TestOpenSourceTarballsAreNotTracked()
{
    const std::string command = "git -C " + RepoPath(".") + " ls-files 3rdparty/open_source";
    const auto trackedFiles = RunCommand(command);

    CheckNoLineContains("tracked open-source dependency archive", trackedFiles, ".tar.gz");
    CheckNoLineContains("tracked open-source dependency archive", trackedFiles, ".tar.xz");
}

void TestCommInitChecksDeviceCommArgsSync()
{
    const std::string path = "src/comm/tilexr_comm.cpp";
    const auto text = ReadFile(path);

    CheckNotContains(path, text, "InitMem();\n    g_localPeerMemMap");
    CheckNotContains(path, text, "    SyncCommArgs();");
    CheckContains(path, text, "ret = SyncCommArgs();");
    CheckContains(path, text, "ret = InitMem();");
}

void TestDedicatedCreditIpcLifecycle()
{
    const std::string cppPath = "src/comm/tilexr_comm.cpp";
    const std::string headerPath = "src/include/comm_args.h";
    const auto cppText = ReadFile(cppPath);
    const auto headerText = ReadFile(headerPath);

    CheckContains(headerPath, headerText,
        "GM_ADDR creditMems[TILEXR_MAX_RANK_SIZE]");
    CheckContains(headerPath, headerText, "CREDIT_IPC_BYTES");
    CheckContains(cppPath, cppText, "TILEXR_ENABLE_CREDIT_IPC");
    CheckContains(cppPath, cppText, "InitCreditIpcMem(pids, sdids)");
    CheckContains(cppPath, cppText, "rtIpcOpenMemory(");
    CheckContains(cppPath, cppText, "CloseCreditIpcMem();");
    CheckContains(cppPath, cppText,
        "FreePeerMem(creditIpcMem_[rank_]);");
}

void TestCWrappersDoNotPublishFailedCommunicators()
{
    const std::string path = "src/comm/comm_wrap.cpp";
    const auto text = ReadFile(path);

    CheckNotContains(path, text, "*comm = c;\n    int ret = c->Init();");
    CheckNotContains(path, text, "*comm = c;\n    int ret = c->InitThread");
    CheckNotContains(path, text, "comms[i] = new (std::nothrow) TileXRComm");
    CheckContains(path, text, "c.release()");
    CheckContains(path, text, "commHolders[i].release()");
}

void TestDumpInitCleansFailedAllocations()
{
    const std::string path = "src/comm/tilexr_comm.cpp";
    const auto text = ReadFile(path);

    CheckContains(path, text, "aclrtFree(dumpAddr);");
    CheckContains(path, text, "std::free(memory);");
}

void TestSocketExchangeUsesDirectConnectionsOnly()
{
    const std::string cppPath = "src/comm/tools/socket/tilexr_sock_exchange.cpp";
    const std::string headerPath = "src/comm/tools/socket/tilexr_sock_exchange.h";
    const auto cppText = ReadFile(cppPath);
    const auto headerText = ReadFile(headerPath);

    CheckNotContains(cppPath, cppText, "StartSecureTunnel");
    CheckNotContains(headerPath, headerText, "StartSecureTunnel");
    CheckNotContains(cppPath, cppText, "popen");
    CheckNotContains(cppPath, cppText, "/usr/bin/ssh");
    CheckNotContains(headerPath, headerText, "FILE* pipe_");
    CheckNotContains(headerPath, headerText, "lockFileDescriptor_");
    CheckContains(cppPath, cppText, "return Connect();");
    CheckContains(cppPath, cppText, "bool envProvided = false;");
    CheckContains(cppPath, cppText, "if (!envProvided) {");
    CheckContains(headerPath, headerText, "while (sent < sendSize)");
    CheckContains(headerPath, headerText, "while (received < recvSize)");
    CheckContains(headerPath, headerText, "sendFlags |= MSG_NOSIGNAL");
    CheckNotContains(cppPath, cppText,
                     "handle->addr.sin.sin_port = htons(TILEXR_DEFAULT_SOCK_PORT + dev + commDomain);");
}

void TestRuntimeEnvDoesNotPrependCannDevlib()
{
    const std::string path = "scripts/common_env.sh";
    const auto text = ReadFile(path);

    CheckNotContains(path, text, "${ASCEND_HOME_PATH}/${TILEXR_OS_ARCH}-linux/devlib");
}

void TestRuntimeEnvUsesReadableDriverShim()
{
    const std::string path = "scripts/common_env.sh";
    const auto text = ReadFile(path);

    CheckContains(path, text, "TILEXR_DRIVER_SHIM_HOME");
    CheckContains(path, text, "[ ! -r \"${ASCEND_DRIVER_PATH}/kernel/inc\" ]");
    CheckContains(path, text, "${ASCEND_HOME_PATH}/${TILEXR_OS_ARCH}-linux/include/driver");
    CheckContains(path, text, "export ASCEND_DRIVER_PATH=${TILEXR_DRIVER_SHIM_HOME}");
}

void TestRootCMakeRespectsAscendDriverOverride()
{
    const std::string path = "CMakeLists.txt";
    const auto text = ReadFile(path);

    CheckNotContains(path, text, "set(ASCEND_DRIVER_PATH /usr/local/Ascend/driver)");
    CheckContains(path, text, "set(_tilexr_default_ascend_driver_path \"$ENV{ASCEND_DRIVER_PATH}\")");
    CheckContains(path, text, "set(ASCEND_DRIVER_PATH \"${_tilexr_default_ascend_driver_path}\" CACHE PATH");
}

void TestPublicTypesKeepHostNamesOutOfCce()
{
    const std::string path = "src/include/tilexr_types.h";
    const auto text = ReadFile(path);

    CheckContains(path, text, "#include <cstdint>");
    CheckContains(path, text,
        "#ifndef __CCE__\n#include <map>\n#include <string>\n#endif");
    CheckContains(path, text,
        "#ifndef __CCE__\nconst std::map<TileXRType, std::string> TILEXR_TYPE2NAME");
}

void TestCommBuildIncludesProfilingHeaders()
{
    const std::string rootPath = "CMakeLists.txt";
    const std::string commPath = "src/comm/CMakeLists.txt";
    const auto rootText = ReadFile(rootPath);
    const auto commText = ReadFile(commPath);

    CheckContains(rootPath, rootText, "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/profiling/");
    CheckContains(commPath, commText, "${ASCEND_HOME_PATH}/${ARCH}-linux/pkg_inc/profiling/");
}

void TestCommInitHonorsUDMAOptOut()
{
    const std::string path = "src/comm/tilexr_comm.cpp";
    const auto text = ReadFile(path);

    CheckContains(path, text, "IsEnvEnabled(\"TILEXR_ENABLE_UDMA\", true)");
    CheckContains(path, text, "allUdmaEnabled");
    CheckContains(path, text, "TILEXR_ENABLE_UDMA must match on every rank");
    CheckContains(path, text, "TileXR UDMA disabled by TILEXR_ENABLE_UDMA");
}

void TestSharedQpDomainIsExplicit()
{
    const std::string apiPath = "src/include/tilexr_api.h";
    const std::string wrapperPath = "src/comm/comm_wrap.cpp";
    const std::string commPath = "src/comm/tilexr_comm.cpp";
    const std::string contextPath = "src/comm/udma/tilexr_udma_context.cpp";
    const auto apiText = ReadFile(apiPath);
    const auto wrapperText = ReadFile(wrapperPath);
    const auto commText = ReadFile(commPath);
    const auto contextText = ReadFile(contextPath);

    CheckContains(apiPath, apiText, "TileXRCommInitRankWithSharedQpDomain");
    CheckContains(wrapperPath, wrapperText,
        "commDomain, minBufferSize, rankSize, rank, true, false, comm");
    CheckContains(wrapperPath, wrapperText,
        "commDomain, bufferSize, rankSize, rank, false, false, comm");
    CheckContains(commPath, commText,
        "options.sharedQpDomain = sharedQpDomain_");
    CheckContains(contextPath, contextText,
        "if (options_.sharedQpDomain)");
    CheckNotContains(apiPath, apiText, "TILEXR_UDMA_SHARED_QP");
}

void TestChipMapCoversObservedAscend950Variants()
{
    const std::string path = "src/comm/tilexr_internal.cpp";
    const auto text = ReadFile(path);

    CheckContains(path, text, "{\"Ascend950DT_9582\", ChipName::CHIP_950}");
    CheckContains(path, text, "{\"Ascend950PR\", ChipName::CHIP_950PR}");
    CheckContains(path, text, "{\"Ascend950PR_9589\", ChipName::CHIP_950PR}");
    CheckContains(path, text, "{\"Ascend950PR_9599\", ChipName::CHIP_950PR}");
    CheckContains(path, text, "chipName.find(\"Ascend950DT_\") == 0");
    CheckContains(path, text, "chipName == ChipName::CHIP_950;");
    CheckContains(path, text, "std::string(value) == \"pid\"");
    CheckContains(path, text, "std::string(value) == \"sdid\"");
    CheckContains(path, text, "rank / localRankSize == peer / localRankSize");

    const std::string commPath = "src/comm/tilexr_comm.cpp";
    const auto commText = ReadFile(commPath);
    CheckContains(commPath, commText, "TILEXR_IPC_PID_MODE");
    CheckContains(commPath, commText, "TILEXR_ENABLE_IPC");
    CheckContains(commPath, commText, "IsEnvEnabled(\"TILEXR_ENABLE_IPC\", true)");
    CheckContains(commPath, commText, "allIpcEnabled");
    CheckContains(commPath, commText, "TILEXR_ENABLE_IPC must match on every rank");
    CheckContains(commPath, commText, "UsePidOnlyIpcForPeer");
    CheckContains(commPath, commText, "rtSetIpcMemPid");
    CheckContains(commPath, commText, "rtSetIpcMemorySuperPodPid");
}

} // namespace

int main()
{
    TestOpenSourceTarballsAreNotTracked();
    TestCommInitChecksDeviceCommArgsSync();
    TestDedicatedCreditIpcLifecycle();
    TestCWrappersDoNotPublishFailedCommunicators();
    TestDumpInitCleansFailedAllocations();
    TestSocketExchangeUsesDirectConnectionsOnly();
    TestRuntimeEnvDoesNotPrependCannDevlib();
    TestRootCMakeRespectsAscendDriverOverride();
    TestPublicTypesKeepHostNamesOutOfCce();
    TestCommBuildIncludesProfilingHeaders();
    TestCommInitHonorsUDMAOptOut();
    TestSharedQpDomainIsExplicit();
    TestChipMapCoversObservedAscend950Variants();

    if (g_failures != 0) {
        std::cerr << g_failures << " TileXR source guard checks failed" << std::endl;
        return 1;
    }
    std::cout << "TileXR source guard checks passed" << std::endl;
    return 0;
}
