// Reads bounded case JSON and sends phase replies over the private control FD.
// Diagnostic logs remain separate; parsing and I/O failures propagate to Worker.
#include "protocol.h"
#include <cerrno>
#include <fstream>
#include <iterator>
#include <unistd.h>
namespace unitest {
void reply(int fd, int rank, const std::string &case_id, const std::string &phase,
    const std::string &status, Json data) {
    Json message = object();
    put(message, "schema", Json(std::string("tilexr.unitest.worker.v1")));
    put(message, "rank", number(rank)); put(message, "case_id", Json(case_id));
    put(message, "phase", Json(phase)); put(message, "status", Json(status));
    put(message, "data", std::move(data));
    const std::string wire = message.serialize() + "\n";
    size_t offset = 0;
    while (offset < wire.size()) {
        const auto n = write(fd, wire.data()+offset, wire.size()-offset);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "controller connection closed");
        offset += static_cast<size_t>(n);
    }
}
Json read_case(const std::string &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input.good(), "cannot open resolved case");
    require(input.tellg() > 0 && input.tellg() <= 65536, "invalid case size");
    input.seekg(0);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Json value;
    const std::string error = picojson::parse(value, text);
    require(error.empty(), "invalid case JSON: " + error);
    return value;
}
} // namespace unitest
