#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

#include "src/common/status.h"
#include "src/common/aligned_allocator.h"
#include "src/common/config.h"
#include "src/scheduler/scheduler_proxy.h"

namespace falconkv {

struct WriteItem {
    uint64_t offset;
    const void* data;
    uint32_t size;
};

struct ReadItem {
    uint64_t offset;
    void* buffer;
    uint32_t size;
};

struct WriteTask {
    uint64_t offset;
    std::vector<uint8_t> data;  // owns the data copy
    std::function<void(Status)> callback;
};

struct StoreKeyRecord;
class AlignedBufferPool;
class BuddyAllocator;
class StoreMetaIndex;
class MetaSyncClient;
class PendingEvictQueue;
class EvictManager;

struct StorePutResult {
    Status status;
    uint64_t offset = 0;
    uint32_t chunk_size = 0;
};

struct StoreGetResult {
    Status status;
    uint32_t size = 0;
};

class FalconKVStore {
public:
    struct Config {
        std::string ssd_path;
        uint32_t store_id = 0;
        uint32_t node_id = 0;
        uint64_t capacity_bytes = 500ULL * 1024 * 1024 * 1024;
        uint32_t chunk_size = 2 * 1024 * 1024;
        uint32_t page_size = 4096;
        uint32_t io_threads = 4;
        uint32_t write_queue_size = 1024;
        bool disable_mtime = true;
        std::string scheduler_uds_path;
        bool scheduler_enabled = true;
        std::string store_rpc_host = "127.0.0.1";
        uint32_t listen_port = 8901;
        std::string meta_addr;     // Meta server address for sync
        uint32_t evict_grace_period_ms = 5000;
        uint32_t evict_check_interval_sec = 60;
        double evict_high_watermark = 0.85;
        double evict_low_watermark = 0.70;
        uint64_t evict_cold_threshold_ms = 300000;

        static Config FromStoreConfig(const StoreConfig& sc);
    };

    explicit FalconKVStore(const Config& config);
    ~FalconKVStore();

    Status Init(const std::string& meta_addr = "");

    // --- Offset-based low-level IO (existing) ---
    Status Write(uint64_t offset, const void* data, uint32_t size);
    Status Read(uint64_t offset, void* buffer, uint32_t size);
    Status BatchWrite(const std::vector<WriteItem>& items);
    Status BatchRead(const std::vector<ReadItem>& items);

    // --- Key-aware high-level API (new) ---
    StorePutResult Put(const std::string& key, const void* data, uint32_t size);
    std::vector<StorePutResult> BatchPut(const std::vector<std::string>& keys,
                                          const std::vector<const void*>& data_ptrs,
                                          const std::vector<uint32_t>& sizes);
    StoreGetResult Get(const std::string& key, void* buffer, uint32_t buffer_size);
    bool Contains(const std::string& key);
    void BatchContains(const std::vector<std::string>& keys,
                       std::vector<StoreKeyRecord>& hits,
                       std::vector<std::string>& misses);
    Status Remove(const std::string& key);
    double GetUsageRatio() const;

    void Close();

    uint32_t store_id() const { return store_id_; }
    uint32_t node_id() const { return config_.node_id; }
    const std::string& data_file() const { return data_file_; }
    uint32_t chunk_size() const { return config_.chunk_size; }
    SchedulerProxy* scheduler_proxy() const { return scheduler_proxy_.get(); }

private:
    Status InitDataFile();
    void* AllocateAlignedBuffer(uint32_t size);
    void FreeAlignedBuffer(void* buf);

    Config config_;
    int data_fd_ = -1;
    uint32_t store_id_;
    std::string data_file_;
    std::vector<std::thread> io_workers_;
    std::atomic<bool> running_{false};
    std::unique_ptr<AlignedBufferPool> buffer_pool_;

    // New: space management + local metadata + meta sync
    std::unique_ptr<BuddyAllocator> allocator_;
    std::unique_ptr<StoreMetaIndex> meta_index_;
    std::unique_ptr<MetaSyncClient> meta_sync_client_;
    std::unique_ptr<PendingEvictQueue> pending_evict_queue_;
    std::unique_ptr<EvictManager> evict_manager_;
    std::unique_ptr<SchedulerProxy> scheduler_proxy_;
};

} // namespace falconkv
