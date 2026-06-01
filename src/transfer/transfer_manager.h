#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include "src/common/status.h"
#include "src/common/config.h"
#include "src/transfer/transfer_channel.h"

namespace falconkv {

class TransferManager {
public:
    explicit TransferManager(const TransferConfig& config);
    ~TransferManager();

    TransferChannel* GetMetaChannel();
    TransferChannel* GetStoreChannel(uint32_t store_id);
    std::vector<TransferChannel*> GetStoreChannels(const std::vector<uint32_t>& store_ids);
    void RegisterStoreAddr(uint32_t store_id, const std::string& addr);
    void CloseAll();

private:
    std::unique_ptr<TransferChannel> CreateChannel(const std::string& addr);

    TransferConfig config_;
    std::unique_ptr<TransferChannel> meta_channel_;
    std::mutex meta_mutex_;
    std::unordered_map<uint32_t, std::vector<std::unique_ptr<TransferChannel>>> store_channels_;
    std::unordered_map<uint32_t, std::string> store_addrs_;
    std::mutex store_mutex_;
};

} // namespace falconkv
