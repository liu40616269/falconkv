#pragma once

#include "falconkv_meta.pb.h"
#include "src/meta/meta_manager.h"

namespace falconkv {

// Server-side implementation of FalconKVMetaService.
// Each RPC method extracts proto request fields, delegates to MetaManager,
// and converts C++ results back to proto response.
class MetaServiceImpl : public FalconKVMetaService {
public:
    explicit MetaServiceImpl(MetaManager* meta_manager);
    ~MetaServiceImpl() override = default;

    void BatchExist(::google::protobuf::RpcController* controller,
                    const BatchExistRequest* request,
                    BatchExistResponse* response,
                    ::google::protobuf::Closure* done) override;

    void BatchLookup(::google::protobuf::RpcController* controller,
                     const BatchLookupRequest* request,
                     BatchLookupResponse* response,
                     ::google::protobuf::Closure* done) override;

    void StoreRegister(::google::protobuf::RpcController* controller,
                       const StoreRegisterRequest* request,
                       StoreRegisterResponse* response,
                       ::google::protobuf::Closure* done) override;

    void ClientHeartbeat(::google::protobuf::RpcController* controller,
                         const ClientHeartbeatRequest* request,
                         ClientHeartbeatResponse* response,
                         ::google::protobuf::Closure* done) override;

    void SyncCommit(::google::protobuf::RpcController* controller,
                    const SyncCommitRequest* request,
                    SyncCommitResponse* response,
                    ::google::protobuf::Closure* done) override;

    void SyncRemove(::google::protobuf::RpcController* controller,
                    const SyncRemoveRequest* request,
                    SyncRemoveResponse* response,
                    ::google::protobuf::Closure* done) override;

private:
    MetaManager* meta_manager_; // not owned
};

} // namespace falconkv
