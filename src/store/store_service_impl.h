#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "falconkv_store.pb.h"
#include "src/store/falconkv_store.h"

#ifdef FALCONKV_HAS_HIXL
#include <hixl/hixl.h>
#endif

namespace falconkv {

/// Server-side implementation of FalconKVStoreService.
/// Each RPC method extracts proto request fields, delegates to FalconKVStore,
/// and converts C++ results back to proto response.
class StoreServiceImpl : public FalconKVStoreService {
public:
    explicit StoreServiceImpl(FalconKVStore* store);
    ~StoreServiceImpl() override;

    void Read(::google::protobuf::RpcController* controller,
              const ReadRequest* request,
              ReadResponse* response,
              ::google::protobuf::Closure* done) override;

    void BatchRead(::google::protobuf::RpcController* controller,
                   const BatchReadRequest* request,
                   BatchReadResponse* response,
                   ::google::protobuf::Closure* done) override;

    void PrepareHixlBatchRead(::google::protobuf::RpcController* controller,
                              const HixlBatchReadRequest* request,
                              HixlBatchReadResponse* response,
                              ::google::protobuf::Closure* done) override;

    void ReleaseHixlRead(::google::protobuf::RpcController* controller,
                         const HixlReleaseRequest* request,
                         HixlReleaseResponse* response,
                         ::google::protobuf::Closure* done) override;

    void GetByKey(::google::protobuf::RpcController* controller,
                  const GetByKeyRequest* request,
                  GetByKeyResponse* response,
                  ::google::protobuf::Closure* done) override;

    void BatchGetByKey(::google::protobuf::RpcController* controller,
                       const BatchGetByKeyRequest* request,
                       BatchGetByKeyResponse* response,
                       ::google::protobuf::Closure* done) override;

    void Ping(::google::protobuf::RpcController* controller,
              const PingRequest* request,
              PongResponse* response,
              ::google::protobuf::Closure* done) override;

private:
    struct HixlReadLease {
        std::vector<void*> buffers;
#ifdef FALCONKV_HAS_HIXL
        std::vector<hixl::MemHandle> handles;
#else
        std::vector<void*> handles;
#endif
    };

#ifdef FALCONKV_HAS_HIXL
    Status EnsureHixlInitialized();
#endif

    FalconKVStore* store_; // not owned
    std::mutex hixl_mutex_;
    std::unordered_map<std::string, HixlReadLease> hixl_leases_;
    std::atomic<uint64_t> hixl_next_token_{1};
#ifdef FALCONKV_HAS_HIXL
    std::unique_ptr<hixl::Hixl> hixl_engine_;
    bool hixl_initialized_ = false;
#endif
};

} // namespace falconkv
