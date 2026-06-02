#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>

#include <atomic>
#include <string>

#include "src/common/config.h"
#include "src/common/logging.h"
#include "src/meta/meta_server.h"

static std::atomic<bool> g_stopped{false};

static void SignalHandler(int signo) {
    // Only use async-signal-safe operations inside signal handlers.
    // LOG() / glog are NOT signal-safe (they use mutexes internally) and
    // can deadlock if the signal interrupts a thread holding glog's lock.
    const char msg[] = "[falconkv_master] Received signal, shutting down...\n";
    ssize_t unused __attribute__((unused)) = write(STDERR_FILENO, msg, sizeof(msg) - 1);
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

    falconkv::FalconKVConfig config = falconkv::ConfigLoader::LoadFromFile(config_file);

    falconkv::InitSharedLogging(config.common.log_dir, argv[0]);

    LOG(INFO) << "[falconkv_master] Loading config from: " << config_file;

    // Create and start MetaServer (must be done BEFORE installing signal
    // handlers, because brpc::Server::Start() triggers global initialization
    // that installs its own SIGTERM handler via signal(), which would override
    // ours if installed too early).
    falconkv::MetaServer server(config.meta);

    falconkv::Status s = server.Start();
    if (!s.ok()) {
        LOG(ERROR) << "[falconkv_master] Failed to start MetaServer: " << s.ToString();
        return 1;
    }

    // Install signal handlers AFTER brpc initialization so that our handlers
    // take precedence over brpc's internal SIGTERM handler.
    InstallSignalHandlers();

    LOG(INFO) << "[falconkv_master] MetaServer started on " << server.ListenAddr();

    // Main loop: sleep until stop signal
    while (!g_stopped.load()) {
        sleep(1);
    }

    LOG(INFO) << "[falconkv_master] Stopping MetaServer...";
    server.Stop();
    LOG(INFO) << "[falconkv_master] Stopped.";

    falconkv::ShutdownSharedLogging();

    return 0;
}
