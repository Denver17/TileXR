/*
 * 功能：读取源码和 CMake，检查 diagnostics 的模块依赖边界及 kernel 注册/安装配置。
 * 调用关系：由 diagnostics CTest 启动，使用 TILEXR_SOURCE_ROOT 定位仓库文件；不执行 kernel。
 * Purpose: Inspect source/CMake text for diagnostics dependency boundaries and kernel registration/install wiring.
 * Calls: Diagnostics CTest supplies TILEXR_SOURCE_ROOT to locate repository files; no kernel is executed.
 */
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
