#include "src/transfer/transfer_manager.h"
#include "src/transfer/brpc_channel.h"

#include <algorithm>
#include <thread>

#include "src/common/logging.h"

namespace falconkv {

TransferManager::TransferManager(const TransferConfig& config)
    : config_(config) {}

TransferManager::~TransferManager() {
    CloseAll();
}

std::unique_ptr<TransferChannel> TransferManager::CreateChannel(const std::string& addr) {
    auto channel = std::make_unique<BrpcChannel>();
    Status s = channel->Connect(addr);
    if (!s.ok()) {
        LOG(ERROR) << "[TransferManager] CreateChannel failed for " << addr
                   << ": " << s.ToString();
        return nullptr;
    }
    LOG(INFO) << "[TransferManager] Created channel to " << addr;
    return channel;
}

TransferChannel* TransferManager::GetMetaChannel() {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    if (meta_channel_ && meta_channel_->IsConnected()) {
        return meta_channel_.get();
    }

    if (config_.meta_addr.empty()) {
        LOG(WARNING) << "[TransferManager] GetMetaChannel: meta_addr is empty";
        return nullptr;
    }

    auto channel = CreateChannel(config_.meta_addr);
    if (!channel) {
        LOG(ERROR) << "[TransferManager] GetMetaChannel: failed to create channel to "
                   << config_.meta_addr;
        return nullptr;
    }

    LOG(INFO) << "[TransferManager] GetMetaChannel: connected to meta at " << config_.meta_addr;
    meta_channel_ = std::move(channel);
    return meta_channel_.get();
}

TransferChannel* TransferManager::GetStoreChannel(uint32_t store_id) {
    std::lock_guard<std::mutex> lock(store_mutex_);

    auto addr_it = store_addrs_.find(store_id);
    if (addr_it == store_addrs_.end()) {
        LOG(WARNING) << "[TransferManager] GetStoreChannel: no address registered for store_id "
                     << store_id;
        return nullptr;
    }

    auto& pool = store_channels_[store_id];
    for (auto& ch : pool) {
        if (ch && ch->IsConnected()) {
            return ch.get();
        }
    }

    auto channel = CreateChannel(addr_it->second);
    if (!channel) {
        LOG(ERROR) << "[TransferManager] GetStoreChannel: failed to create channel to store "
                   << store_id << " at " << addr_it->second;
        return nullptr;
    }

    LOG(INFO) << "[TransferManager] GetStoreChannel: created new channel to store "
              << store_id << " at " << addr_it->second;
    pool.push_back(std::move(channel));
    return pool.back().get();
}

std::vector<TransferChannel*> TransferManager::GetStoreChannels(
        const std::vector<uint32_t>& store_ids) {
    std::vector<TransferChannel*> result;
    result.reserve(store_ids.size());
    for (uint32_t id : store_ids) {
        auto* ch = GetStoreChannel(id);
        if (ch) {
            result.push_back(ch);
        }
    }
    return result;
}

void TransferManager::RegisterStoreAddr(uint32_t store_id, const std::string& addr) {
    std::lock_guard<std::mutex> lock(store_mutex_);
    store_addrs_[store_id] = addr;
    LOG(INFO) << "[TransferManager] RegisterStoreAddr: store_id=" << store_id << ", addr=" << addr;
}

void TransferManager::CloseAll() {
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        if (meta_channel_) {
            meta_channel_->Disconnect();
            meta_channel_.reset();
        }
    }
    {
        std::lock_guard<std::mutex> lock(store_mutex_);
        for (auto& [id, pool] : store_channels_) {
            for (auto& ch : pool) {
                if (ch) {
                    ch->Disconnect();
                }
            }
        }
        store_channels_.clear();
        store_addrs_.clear();
    }
}

} // namespace falconkv
