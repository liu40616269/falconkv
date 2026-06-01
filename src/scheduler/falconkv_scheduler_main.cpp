#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>

#include <atomic>
#include <string>

#include "src/common/config.h"
#include "src/common/logging.h"
#include "src/scheduler/scheduler_server.h"

static std::atomic<bool> g_stopped{false};

static void SignalHandler(int signo) {
    LOG(WARNING) << "[falconkv_scheduler] Received signal " << signo << ", shutting down...";
    g_stopped.store(true);
}

static void InstallSignalHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SignalHandler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

int main(int argc, char* argv[]) {
#ifdef FALCONKV_HAS_GLOG
    google::InitGoogleLogging(argv[0]);
#endif

    // Determine config file path:
    // 1. Command-line argument (if provided)
    // 2. FALCONKV_CONFIG_FILE environment variable
    // 3. Default path
    std::string config_file;
    if (argc > 1) {
        config_file = argv[1];
    } else {
        const char* env_path = std::getenv("FALCONKV_CONFIG_FILE");
        if (env_path && env_path[0] != '\0') {
            config_file = env_path;
        } else {
            config_file = "config/falconkv.json";
        }
    }

    LOG(INFO) << "[falconkv_scheduler] Loading config from: " << config_file;
    falconkv::FalconKVConfig config = falconkv::ConfigLoader::LoadFromFile(config_file);

    InstallSignalHandlers();

    // Create and start SchedulerServer
    falconkv::SchedulerServer server(config.scheduler);

    falconkv::Status s = server.Start();
    if (!s.ok()) {
        LOG(ERROR) << "[falconkv_scheduler] Failed to start SchedulerServer: " << s.ToString();
        return 1;
    }

    LOG(INFO) << "[falconkv_scheduler] SchedulerServer started on " << server.UDSPath();

    // Main loop: sleep until stop signal
    while (!g_stopped.load()) {
        sleep(1);
    }

    LOG(INFO) << "[falconkv_scheduler] Stopping SchedulerServer...";
    server.Stop();
    LOG(INFO) << "[falconkv_scheduler] Stopped.";

#ifdef FALCONKV_HAS_GLOG
    google::ShutdownGoogleLogging();
#endif

    return 0;
}
