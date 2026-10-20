#include <tilexr/profiling.h>
int library(); int other();
int main() { TileXR::Profiling::CaptureSnapshot s; std::vector<uint8_t> bytes;
 return library()==1 && other()==72 && TileXR::Profiling::EncodeV1(s,&bytes).ok() && bytes.size()==48 ? 0:1; }
