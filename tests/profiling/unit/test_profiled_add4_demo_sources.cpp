#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int gFailures = 0;

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
    if (!input) {
        std::cerr << "failed to open " << path << std::endl;
        ++gFailures;
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void Contains(const std::string& path, const std::string& text, const std::string& needle)
{
    if (text.find(needle) == std::string::npos) {
        std::cerr << path << " missing: " << needle << std::endl;
        ++gFailures;
    }
}

void Excludes(const std::string& path, const std::string& text, const std::string& needle)
{
    if (text.find(needle) != std::string::npos) {
        std::cerr << path << " should not contain: " << needle << std::endl;
        ++gFailures;
    }
}

void Ordered(const std::string& path, const std::string& text,
    const std::string& first, const std::string& second)
{
    const std::size_t firstPosition = text.find(first);
    const std::size_t secondPosition = text.find(second);
    if (firstPosition == std::string::npos || secondPosition == std::string::npos ||
        firstPosition >= secondPosition) {
        std::cerr << path << " expected '" << first << "' before '" << second << "'"
                  << std::endl;
        ++gFailures;
    }
}

std::size_t Count(const std::string& text, const std::string& needle)
{
    std::size_t count = 0U;
    std::size_t position = 0U;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

} // namespace

int main()
{
    const std::string kernelPath = "tests/profiling/demo/profiled_add4_kernel.cpp";
    const std::string hostPath = "tests/profiling/demo/profiled_add4_host.cpp";
    const std::string mappingPath = "tests/profiling/demo/profiled_add4.mapping.json";
    const std::string runnerPath = "tests/profiling/demo/run_demo.sh";
    const std::string headerPath = "src/include/tilexr_profiling.h";
    const std::string commArgsPath = "src/include/comm_args.h";
    const std::string commPath = "src/comm/tilexr_comm.cpp";
    const std::string wrapperPath = "src/comm/comm_wrap.cpp";
    const std::string kernel = ReadFile(kernelPath);
    const std::string host = ReadFile(hostPath);
    const std::string mapping = ReadFile(mappingPath);
    const std::string runner = ReadFile(runnerPath);
    const std::string header = ReadFile(headerPath);
    const std::string commArgs = ReadFile(commArgsPath);
    const std::string comm = ReadFile(commPath);
    const std::string wrapper = ReadFile(wrapperPath);

    Contains(headerPath, header, "class ProfilingRecorder");
    Contains(headerPath, header, "tilexrProfilingEventStamps_");
    Contains(headerPath, header, "tilexrProfilingEventIds_");
    Contains(headerPath, header, "void RecordMarker()");
    Contains(headerPath, header, "void Flush(");
    Contains(headerPath, header, "class DirectProfilingWriter");
    Contains(headerPath, header, "void RecordAt(");
    Contains(headerPath, header, "recordCount_[0] = launchBase_ + cursor_");
    Excludes(headerPath, header, "const uint32_t occupied = recordCount[0]");
    Excludes(headerPath, header, "DataCopy");
    Excludes(headerPath, header, "SetFlag");
    Excludes(headerPath, header, "WaitFlag");
    Excludes(headerPath, header, "static uint64_t g_tilexrProfiling");
    Contains(commArgsPath, commArgs, "tilexrProfilingTimestampsGM");
    Contains(commArgsPath, commArgs, "tilexrProfilingEventIdsGM");
    Contains(commArgsPath, commArgs, "tilexrProfilingRecordCountsGM");
    Contains(commPath, comm, "WriteTileXRProfilingRecords");
    Contains(wrapperPath, wrapper, "c->FinalizeProfiling()");
    Ordered(wrapperPath, wrapper, "c->FinalizeProfiling()", "delete c;");

    Contains(kernelPath, kernel, "#include \"tilexr_profiling.h\"");
    Contains(kernelPath, kernel, "TileXR::ProfilingRecorder<kRecordCount>");
    Contains(kernelPath, kernel,
        "tilexrProfiler.RecordMarker<0U, kEventTotalBegin>()");
    Contains(kernelPath, kernel, "tilexrProfiler.Flush(");
    Contains(kernelPath, kernel, "AscendC::Add(outputLocal");
    Excludes(kernelPath, kernel, "uint64_t timestamps[");
    Excludes(kernelPath, kernel, "uint32_t eventIds[");
    Excludes(kernelPath, kernel, "void RecordMarker(");
    if (Count(kernel, "tilexrProfiler.RecordMarker<") != 20U) {
        std::cerr << kernelPath << " must emit exactly 20 fixed marker calls" << std::endl;
        ++gFailures;
    }
    Ordered(kernelPath, kernel,
        "tilexrProfiler.RecordMarker<19U, kEventTotalEnd>()",
        "tilexrProfiler.Flush(");

    Contains(hostPath, host, "TileXRCommInitRankLocal");
    Contains(hostPath, host, "TileXRGetCommArgsDev");
    Contains(hostPath, host, "TileXRCommDestroy");
    Contains(hostPath, host, "rtKernelLaunchWithFlagV2");
    Excludes(hostPath, host, "WriteMetadata");
    Excludes(hostPath, host, "WriteRecords");
    Excludes(hostPath, host, "CaptureId");
    Excludes(hostPath, host, "capture.mapping.json");
    Excludes(hostPath, host, "capture.records.bin");

    Contains(mappingPath, mapping, "tilexr_profiling_mapping.v1");
    Contains(mappingPath, mapping, "\"parent\"");
    Contains(mappingPath, mapping, "\"instances\"");
    Contains(mappingPath, mapping, "partial_sum");
    Excludes(mappingPath, mapping, "capture_id");
    Excludes(mappingPath, mapping, "streams");
    Excludes(mappingPath, mapping, "launches");
    Contains(runnerPath, runner, "TILEXR_PROFILING_CAPTURE_ID");
    Contains(runnerPath, runner, "TILEXR_PROFILING_RECORDS_PATH");
    Contains(runnerPath, runner, "No running processes found in NPU");
    Contains(runnerPath, runner, "python3 -m tools.profiling");

    if (gFailures == 0) {
        std::cout << "Profiled Add4 demo source checks passed" << std::endl;
    }
    return gFailures == 0 ? 0 : 1;
}
