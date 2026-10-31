#include <dirent.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

std::string RepoPath(const std::string &path)
{
    return std::string(TILEXR_SOURCE_ROOT) + "/" + path;
}

std::string ReadFile(const std::string &path)
{
    std::ifstream input(RepoPath(path).c_str());
    if (!input.is_open()) {
        std::cerr << "failed to open " << path << std::endl;
        ++failures;
        return {};
    }
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void Require(const std::string &path, const std::string &text, const std::string &needle)
{
    if (text.find(needle) == std::string::npos) {
        std::cerr << path << " missing: " << needle << std::endl;
        ++failures;
    }
}

void Forbid(const std::string &path, const std::string &text, const std::string &needle)
{
    if (text.find(needle) != std::string::npos) {
        std::cerr << path << " unexpectedly contains: " << needle << std::endl;
        ++failures;
    }
}

void ScanDirectory(const std::string &path)
{
    DIR *directory = opendir(RepoPath(path).c_str());
    if (directory == nullptr) {
        std::cerr << "failed to open " << path << std::endl;
        ++failures;
        return;
    }
    while (dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string child = path + "/" + name;
        if (entry->d_type == DT_DIR) {
            ScanDirectory(child);
        } else if (entry->d_type == DT_REG) {
            const std::string text = ReadFile(child);
            Forbid(child, text, "src/collectives");
            Forbid(child, text, "tilexr-collectives");
            Forbid(child, text, "src/ep");
            Forbid(child, text, "src/moonep");
        }
    }
    closedir(directory);
}

} // namespace

int main()
{
    const std::string cmakePath = "src/diagnostics/CMakeLists.txt";
    const std::string cmake = ReadFile(cmakePath);
    Require(cmakePath, cmake, "add_library(tilexr-diagnostics SHARED");
    Require(cmakePath, cmake, "PUBLIC tile-comm");

    const std::string rootCmakePath = "CMakeLists.txt";
    const std::string rootCmake = ReadFile(rootCmakePath);
    Require(rootCmakePath, rootCmake, "tilexr_diagnostics_install_smoke");
    Require(rootCmakePath, rootCmake,
            "tests/diagnostics/cmake/install_prefix_smoke.cmake.in");

    const std::string hostPath = "src/diagnostics/host/clock_kernel.cpp";
    const std::string host = ReadFile(hostPath);
    Require(hostPath, host, "rtDevBinaryRegister");
    Require(hostPath, host, "rtFunctionRegister");
    Require(hostPath, host, "rtKernelLaunchWithFlagV2");
    Forbid(hostPath, host, "kernel<<<");

    const std::string kernelPath = "src/diagnostics/kernels/clock_sync_kernel.h";
    const std::string kernel = ReadFile(kernelPath);
    Require(kernelPath, kernel, "AscendC::GetSystemCycle()");
    Require(kernelPath, kernel, "TILEXR_CLOCK_SYNC_READY");
    Require(kernelPath, kernel, "TILEXR_CLOCK_SYNC_REQUEST");
    Require(kernelPath, kernel, "TILEXR_CLOCK_SYNC_RESPONSE");
    Require(kernelPath, kernel, "TILEXR_CLOCK_SYNC_TIMESTAMP");
    Require(kernelPath, kernel, "TILEXR_CLOCK_SYNC_ACK");
    Require(kernelPath, kernel, "warmupSampleCount");
    Require(kernelPath, kernel, "GM_ADDR requestSlot = rootMemory + slotOffset");
    Require(kernelPath, kernel, "ClockSyncAckSlotOffset");
    Require(kernelPath, kernel, "ClockSyncTimestampSlotOffset");
    Require(kernelPath, kernel, "TileXRStragglerWriteGm(requestSlot");
    Require(kernelPath, kernel, "TileXRStragglerWriteGm(timestampSlot");
    Require(kernelPath, kernel, "StragglerReadProtocolMatches");
    Require(kernelPath, kernel, "t1 = readStart + (readEnd - readStart) / 2u");
    Require(kernelPath, kernel, "t3 = readStart + (readEnd - readStart) / 2u");
    Require(kernelPath, kernel, "requestWriteStart + (requestWriteEnd - requestWriteStart) / 2u");
    Require(kernelPath, kernel,
            "responseWriteStart + (responseWriteEnd - responseWriteStart) / 2u");

    const std::string sessionPath = "src/diagnostics/host/straggler_session.cpp";
    const std::string session = ReadFile(sessionPath);
    Require(sessionPath, session, "nextMagic(session->comm, &magic)");

    ScanDirectory("src/diagnostics");

    const std::string epPath = "src/ep/CMakeLists.txt";
    const std::string ep = ReadFile(epPath);
    Require(epPath, ep, "tilexr-diagnostics");
    Forbid(epPath, ep, "tilexr-collectives");

    const std::string moonEpPath = "src/moonep/CMakeLists.txt";
    const std::string moonEp = ReadFile(moonEpPath);
    Require(moonEpPath, moonEp, "tilexr-diagnostics");
    Forbid(moonEpPath, moonEp, "tilexr-collectives");
    return failures == 0 ? 0 : 1;
}
