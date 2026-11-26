// Windows: ask hybrid-graphics laptops for the discrete GPU (NVIDIA Optimus and AMD PowerXpress read these exports from
// the executable at start), as tools/play.sh's PRIME offload does on Linux.
#ifdef _WIN32
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
