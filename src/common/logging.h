#pragma once

#include <string>

#ifdef FALCONKV_HAS_GLOG
#include <glog/logging.h>
#endif

namespace falconkv {

/// Initialize shared logging: all severity levels are written to a single
/// file `<log_dir>/falconkv.log` instead of per-level files.
/// When log_dir is empty, logs go to stderr only.
void InitSharedLogging(const std::string& log_dir, const char* argv0);

/// Shutdown glog (only call from standalone processes, not shared libraries).
void ShutdownSharedLogging();

} // namespace falconkv

#ifndef FALCONKV_HAS_GLOG
#include <iostream>
// Fallback for builds without glog
#undef LOG
#undef VLOG
#undef DLOG
#define LOG(severity) std::cerr << "[" #severity "] "
#define VLOG(level) if (false) std::cerr
#define DLOG(severity) if (false) std::cerr
#endif
