#pragma once

namespace spqr {
constexpr const char* appName = "Circus Simulator";
constexpr unsigned initialWindowWidth = 1200;
constexpr unsigned initialWindowHeight = 900;
constexpr const char* frameworkConfigPath = "resources/config/framework_config.yaml";
constexpr const char* pathsConfigPath = "resources/config/path_constants.yaml";
constexpr const char* sharedMemoryPath = "/dev/shm/circus_ipc/";
constexpr int frameworkCommunicationPort = 5555;
constexpr int restartExitCode = 42;  // Leaving the event loop with it makes main() start Circus again from scratch
}  // namespace spqr
