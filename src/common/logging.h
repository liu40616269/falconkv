#pragma once

#ifdef FALCONKV_HAS_GLOG
#include <glog/logging.h>
#else
#include <iostream>

#define LOG(severity) std::cerr << "[" #severity "] "
#define VLOG(level) if (false) std::cerr
#define DLOG(severity) if (false) std::cerr
#endif
