#include <tilexr/profiling/file.h>
#include <tilexr/profiling.h>
int library() { TileXR::Profiling::CaptureConfig c; TileXR::Profiling::Layout l; return TileXR::Profiling::BuildLayout(c,&l).ok() ? 1 : 0; }
