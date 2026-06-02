#include "src/common/logging.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>

namespace falconkv {

/// Extract the basename from argv0 (e.g. "./build/falconkv_master" → "falconkv_master").
static std::string Basename(const char* path) {
    std::string tmp(path);
    char* base = basename(const_cast<char*>(tmp.c_str()));
    return std::string(base);
}

/// Build timestamp string: YYYYMMDD-HHMMSS
static std::string TimestampString() {
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_buf);
    return std::string(buf);
}

void InitSharedLogging(const std::string& log_dir, const char* argv0) {
#ifdef FALCONKV_HAS_GLOG
    // Send all severity levels to stderr instead of per-level files.
    FLAGS_logtostderr = true;
    FLAGS_colorlogtostderr = false;
    FLAGS_minloglevel = 0;

    // google::InitGoogleLogging must only be called once per process.
    static bool glog_initialized = false;
    if (!glog_initialized) {
        google::InitGoogleLogging(argv0);
        glog_initialized = true;
    }

    // freopen is called every time so callers can switch log files
    // (e.g. per-test-case log directories).
    if (!log_dir.empty()) {
        mkdir(log_dir.c_str(), 0755);

        // <log_dir>/<process_name>_<pid>_<timestamp>.log
        // e.g. falconkv_master_1122990_20260602-102047.log
        std::string process_name = Basename(argv0);
        std::string log_filename = process_name + "_" +
                                   std::to_string(getpid()) + "_" +
                                   TimestampString() + ".log";
        std::string log_path = log_dir + "/" + log_filename;
        FILE* f = freopen(log_path.c_str(), "a", stderr);
        if (f) {
            setvbuf(stderr, nullptr, _IOLBF, 0);
        }
    }
#else
    (void)log_dir;
    (void)argv0;
#endif
}

void ShutdownSharedLogging() {
#ifdef FALCONKV_HAS_GLOG
    google::ShutdownGoogleLogging();
#endif
}

} // namespace falconkv
