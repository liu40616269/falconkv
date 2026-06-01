#include "src/store/store_rpc_client_manager.h"

namespace falconkv {

StoreRpcClientManager::~StoreRpcClientManager() {
    CloseAll();
}

StoreRpcClient* StoreRpcClientManager::GetOrCreate(const std::string& addr) {
    if (addr.empty()) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = clients_.find(addr);
    if (it != clients_.end() && it->second->IsConnected()) {
        return it->second.get();
    }

    // Create a new client
    auto client = std::make_unique<StoreRpcClient>();
    Status s = client->Connect(addr);
    if (!s.ok()) {
        return nullptr;
    }

    auto* raw = client.get();
    clients_[addr] = std::move(client);
    return raw;
}

void StoreRpcClientManager::CloseAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.clear();
}

} // namespace falconkv
