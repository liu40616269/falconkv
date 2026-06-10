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

} // namespace falconkv
