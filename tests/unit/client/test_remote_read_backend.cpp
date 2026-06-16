#include "src/client/remote_read_backend.h"

#include <gtest/gtest.h>

namespace falconkv {

namespace {

RemoteReadRequest MakeRequest() {
    RemoteReadRequest request;
    request.store_id = 7;
    request.store_addr = "127.0.0.1:8901";
    request.offsets = {0};
    request.sizes = {4};
    static char buffer[4];
    request.buffers = {buffer};
    return request;
}

} // namespace

TEST(RemoteReadBackend, BrpcBackendRequiresRpcManager) {
    BrpcRemoteReadBackend backend(nullptr);

    std::vector<int32_t> results;
    Status status = backend.BatchRead(MakeRequest(), results);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), Status::kInvalidArg);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0], -1);
}

TEST(RemoteReadBackend, HixlBackendCanDisableBrpcFallback) {
    TransferConfig config;
    config.data_protocol = "hixl";
    config.hixl_fallback_to_brpc = false;
    HixlRemoteReadBackend backend(nullptr, config);

    std::vector<int32_t> results;
    Status status = backend.BatchRead(MakeRequest(), results);

    EXPECT_FALSE(status.ok());
#ifdef FALCONKV_HAS_HIXL
    EXPECT_EQ(status.code(), Status::kInvalidArg);
#else
    EXPECT_EQ(status.code(), Status::kNotSupported);
#endif
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0], -1);
}

TEST(RemoteReadBackend, RejectsInvalidRequestShape) {
    TransferConfig config;
    auto backend = CreateRemoteReadBackend(nullptr, config);

    RemoteReadRequest request = MakeRequest();
    request.sizes.clear();

    std::vector<int32_t> results;
    Status status = backend->BatchRead(request, results);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), Status::kInvalidArg);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0], -1);
}

TEST(RemoteReadBackend, ParsesHostNetworkHixlConfig) {
    const char* json = R"json({
        "store": {
            "hixl_engine_addr": "10.0.0.2:9901",
            "hixl_local_comm_res": "{\"version\":\"1.3\",\"endpoint_list\":[{\"protocol\":\"roce\",\"comm_id\":\"10.0.0.2\",\"placement\":\"host\"}]}",
            "hixl_protocol_desc": "roce:host",
            "hixl_rdma_traffic_class": 132,
            "hixl_rdma_service_level": 4
        },
        "transfer": {
            "data_protocol": "hixl",
            "hixl_local_engine": "10.0.0.1:9902",
            "hixl_local_comm_res": "{\"version\":\"1.3\",\"endpoint_list\":[{\"protocol\":\"roce\",\"comm_id\":\"10.0.0.1\",\"placement\":\"host\"}]}",
            "hixl_protocol_desc": "roce:host",
            "hixl_timeout_ms": 7000
        }
    })json";

    FalconKVConfig config = ConfigLoader::LoadFromString(json);

    EXPECT_EQ(config.store.hixl_engine_addr, "10.0.0.2:9901");
    EXPECT_NE(config.store.hixl_local_comm_res.find("\"placement\":\"host\""),
              std::string::npos);
    EXPECT_EQ(config.store.hixl_protocol_desc, "roce:host");
    EXPECT_EQ(config.store.hixl_rdma_traffic_class, 132);
    EXPECT_EQ(config.store.hixl_rdma_service_level, 4);
    EXPECT_EQ(config.transfer.data_protocol, "hixl");
    EXPECT_EQ(config.transfer.hixl_local_engine, "10.0.0.1:9902");
    EXPECT_NE(config.transfer.hixl_local_comm_res.find("\"protocol\":\"roce\""),
              std::string::npos);
    EXPECT_EQ(config.transfer.hixl_protocol_desc, "roce:host");
    EXPECT_EQ(config.transfer.hixl_timeout_ms, 7000);
}

} // namespace falconkv
