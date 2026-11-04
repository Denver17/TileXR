// Declares resolved-case loading and rank/case/phase/status reply serialization.
#pragma once
#include "json.h"
namespace unitest {
void reply(int fd, int rank, const std::string &case_id, const std::string &phase,
    const std::string &status, Json data = object());
Json read_case(const std::string &path);
} // namespace unitest
