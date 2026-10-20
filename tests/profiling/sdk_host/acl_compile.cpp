#include <tilexr/profiling.h>
int main() { TileXR::Profiling::AclCapture capture; return capture.Close().ok() ? 0 : 1; }
