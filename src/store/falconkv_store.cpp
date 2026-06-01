#include "src/store/falconkv_store.h"
#include "src/store/aligned_buffer_pool.h"
#include "src/store/store_meta_index.h"
#include "src/store/meta_sync_client.h"
#include "src/store/pending_evict_queue.h"
#include "src/store/evict_manager.h"
#include "src/common/buddy_allocator.h"
#include "src/common/logging.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <sys/ioctl.h>

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <algorithm>
#include <future>
#include <chrono>

namespace falconkv {

// ---------------------------------------------------------------------------
// Local helpers that delegate to AlignedAllocator
// ---------------------------------------------------------------------------
static inline uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return AlignedAllocator::AlignUp(value, alignment);
}

static inline bool IsAligned(uint64_t value, uint64_t alignment) {
    return (value & (alignment - 1)) == 0;
}

static inline bool IsPtrAligned(const void* ptr, size_t alignment) {
    return AlignedAllocator::IsAligned(ptr, alignment);
}

// ---------------------------------------------------------------------------
// Config::FromStoreConfig
// ---------------------------------------------------------------------------
FalconKVStore::Config FalconKVStore::Config::FromStoreConfig(const StoreConfig& sc) {
    Config cfg;
    cfg.ssd_path = sc.ssd_path;
    cfg.store_id = sc.store_id;
    cfg.node_id = sc.node_id;
    cfg.capacity_bytes = sc.capacity_bytes;
    cfg.chunk_size = sc.chunk_size;
    cfg.page_size = sc.page_size;
    cfg.io_threads = sc.io_threads;
    cfg.scheduler_enabled = sc.scheduler_enabled;
    cfg.scheduler_uds_path = sc.scheduler_uds_path;
    cfg.store_rpc_host = sc.store_rpc_host;
    cfg.listen_port = sc.listen_port;
    cfg.meta_addr = sc.meta_addr;
    cfg.evict_grace_period_ms = sc.evict_grace_period_ms;
    cfg.evict_check_interval_sec = sc.evict_check_interval_sec;
    cfg.evict_high_watermark = sc.evict_high_watermark;
    cfg.evict_low_watermark = sc.evict_low_watermark;
    cfg.evict_cold_threshold_ms = sc.evict_cold_threshold_ms;
    return cfg;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
FalconKVStore::FalconKVStore(const Config& config)
    : config_(config), store_id_(config.store_id) {
    data_file_ = config_.ssd_path + "/kv_data_" + std::to_string(store_id_);
}

FalconKVStore::~FalconKVStore() {
    Close();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
Status FalconKVStore::Init(const std::string& meta_addr) {
    // Create the SSD directory if it does not exist.
    struct stat st;
    if (stat(config_.ssd_path.c_str(), &st) != 0) {
        if (mkdir(config_.ssd_path.c_str(), 0755) != 0) {
            LOG(ERROR) << "[FalconKVStore] Failed to create SSD directory: "
                       << config_.ssd_path << ": " << strerror(errno);
            return Status::IoError("Failed to create SSD directory: " +
                                   config_.ssd_path + ": " + strerror(errno));
        }
    }

    Status s = InitDataFile();
    if (!s.ok()) {
        LOG(ERROR) << "[FalconKVStore] InitDataFile failed: " << s.ToString();
        return s;
    }

    // Create the aligned buffer pool for DirectIO operations.
    buffer_pool_ = std::make_unique<AlignedBufferPool>(
        config_.chunk_size, config_.page_size, config_.io_threads * 2);

    // Initialize BuddyAllocator for space management.
    uint32_t chunk_pages = config_.chunk_size / config_.page_size;
    if (chunk_pages == 0) chunk_pages = 1;
    allocator_ = std::make_unique<BuddyAllocator>(
        config_.capacity_bytes, config_.page_size, chunk_pages);

    // Initialize local metadata index.
    meta_index_ = std::make_unique<StoreMetaIndex>();

    // Initialize MetaSyncClient.
    meta_sync_client_ = std::make_unique<MetaSyncClient>();
    std::string addr = meta_addr.empty() ? config_.meta_addr : meta_addr;
    meta_sync_client_->SetStoreInfo(store_id_, config_.node_id, data_file_,
                                     config_.capacity_bytes, config_.chunk_size);
    meta_sync_client_->SetMetaIndex(meta_index_.get());
    meta_sync_client_->SetStoreRpcAddr(config_.store_rpc_host, config_.listen_port);
    meta_sync_client_->Connect(addr);  // Connect triggers FullResync on success.
    meta_sync_client_->StartReconnectLoop(5);

    // Initialize PendingEvictQueue for deferred space reclamation.
    pending_evict_queue_ = std::make_unique<PendingEvictQueue>(
        config_.evict_grace_period_ms, allocator_.get());

    // Initialize EvictManager for automatic eviction.
    EvictManager::Config evict_cfg;
    evict_cfg.check_interval_sec = config_.evict_check_interval_sec;
    evict_cfg.high_watermark = config_.evict_high_watermark;
    evict_cfg.low_watermark = config_.evict_low_watermark;
    evict_cfg.cold_threshold_ms = config_.evict_cold_threshold_ms;
    evict_cfg.store_id = store_id_;
    evict_manager_ = std::make_unique<EvictManager>(
        evict_cfg, meta_index_.get(), meta_sync_client_.get(),
        pending_evict_queue_.get(), allocator_.get());

    running_.store(true);

    // Initialize SchedulerProxy if enabled
    if (config_.scheduler_enabled && !config_.scheduler_uds_path.empty()) {
        scheduler_proxy_ = std::make_unique<SchedulerProxy>(config_.scheduler_uds_path);
    }

    // Start background threads after running_ is set.
    pending_evict_queue_->Start();
    evict_manager_->Start();

    return Status::OK();
}

// ---------------------------------------------------------------------------
// InitDataFile - open data file with O_DIRECT, preallocate capacity
// ---------------------------------------------------------------------------
Status FalconKVStore::InitDataFile() {
    int flags = O_CREAT | O_RDWR | O_DIRECT;
    if (config_.disable_mtime) {
        flags |= O_NOATIME;
    }

    data_fd_ = open(data_file_.c_str(), flags, 0644);
    if (data_fd_ < 0) {
        LOG(ERROR) << "[FalconKVStore] Failed to open data file: " << data_file_
                   << ": " << strerror(errno);
        return Status::IoError("Failed to open data file: " + data_file_ +
                               ": " + strerror(errno));
    }

    // Preallocate the file to the configured capacity.
    int rc = posix_fallocate(data_fd_, 0, static_cast<off_t>(config_.capacity_bytes));
    if (rc != 0) {
        // Some filesystems do not support fallocate with O_DIRECT; try truncate.
        if (ftruncate(data_fd_, static_cast<off_t>(config_.capacity_bytes)) != 0) {
            LOG(ERROR) << "[FalconKVStore] Failed to preallocate data file: " << data_file_
                       << ": fallocate error=" << strerror(rc)
                       << ", ftruncate error=" << strerror(errno);
            close(data_fd_);
            data_fd_ = -1;
            return Status::IoError("Failed to preallocate data file: " +
                                   data_file_ + ": " + strerror(rc));
        }
    }

    // Advise sequential access for write patterns.
    posix_fadvise(data_fd_, 0, 0, POSIX_FADV_DONTNEED);

    return Status::OK();
}

// ---------------------------------------------------------------------------
// AllocateAlignedBuffer / FreeAlignedBuffer
// ---------------------------------------------------------------------------
void* FalconKVStore::AllocateAlignedBuffer(uint32_t size) {
    if (buffer_pool_ && buffer_pool_->buffer_size() >= size) {
        return buffer_pool_->Get();
    }
    return AlignedAllocator::Allocate(config_.page_size, size);
}

void FalconKVStore::FreeAlignedBuffer(void* buf) {
    AlignedAllocator::Free(buf);
}

// ---------------------------------------------------------------------------
// Write - single write with DirectIO alignment handling
// ---------------------------------------------------------------------------
Status FalconKVStore::Write(uint64_t offset, const void* data, uint32_t size) {
    if (data_fd_ < 0) {
        LOG(ERROR) << "[FalconKVStore] Write: data file not initialized";
        return Status::IoError("Data file not initialized");
    }
    if (!data || size == 0) {
        LOG(ERROR) << "[FalconKVStore] Write: null data or zero size, offset=" << offset;
        return Status::InvalidArg("Write: null data or zero size");
    }

    uint32_t page_size = config_.page_size;

    // Fast path: offset and size are already page-aligned, and data is
    // sufficiently aligned for O_DIRECT.
    bool offset_aligned = IsAligned(offset, page_size);
    bool size_aligned = IsAligned(size, page_size);
    uintptr_t data_addr = reinterpret_cast<uintptr_t>(data);
    bool data_aligned = IsAligned(data_addr, page_size);

    if (offset_aligned && size_aligned && data_aligned) {
        // Direct write without copying.
        ssize_t written = pwrite(data_fd_, data, size, static_cast<off_t>(offset));
        if (written != static_cast<ssize_t>(size)) {
            LOG(ERROR) << "[FalconKVStore] Write failed at offset " << offset
                       << ", size=" << size << ": " << strerror(errno);
            return Status::IoError("Write failed at offset " + std::to_string(offset) +
                                   ": " + strerror(errno));
        }
        return Status::OK();
    }

    // Slow path: need an aligned buffer to satisfy O_DIRECT requirements.
    uint64_t aligned_offset = offset & ~(static_cast<uint64_t>(page_size) - 1);
    uint64_t end = offset + size;
    uint64_t aligned_end = AlignUp(end, page_size);
    uint32_t aligned_size = static_cast<uint32_t>(aligned_end - aligned_offset);

    void* aligned_buf = AlignedAllocator::Allocate(page_size, aligned_size);
    if (!aligned_buf) {
        LOG(ERROR) << "[FalconKVStore] Write: failed to allocate aligned buffer, size="
                   << aligned_size;
        return Status::IoError("Write: failed to allocate aligned buffer");
    }

    // Read-modify-write: first read the existing data in the head/tail regions
    // that we are not fully overwriting.
    bool need_head_fill = (offset > aligned_offset);
    bool need_tail_fill = (end < aligned_end);

    if (need_head_fill || need_tail_fill) {
        ssize_t rd = pread(data_fd_, aligned_buf, aligned_size,
                           static_cast<off_t>(aligned_offset));
        if (rd < 0) {
            AlignedAllocator::Free(aligned_buf);
            LOG(ERROR) << "[FalconKVStore] Write: read-modify-write pread failed at offset "
                       << aligned_offset << ": " << strerror(errno);
            return Status::IoError("Write: read-modify-write pread failed: " +
                                   std::string(strerror(errno)));
        }
    }

    // Copy user data into the aligned buffer at the correct position.
    uint32_t copy_offset = static_cast<uint32_t>(offset - aligned_offset);
    memcpy(static_cast<uint8_t*>(aligned_buf) + copy_offset, data, size);

    // Write the full aligned block.
    ssize_t written = pwrite(data_fd_, aligned_buf, aligned_size,
                             static_cast<off_t>(aligned_offset));
    AlignedAllocator::Free(aligned_buf);

    if (written != static_cast<ssize_t>(aligned_size)) {
        LOG(ERROR) << "[FalconKVStore] Write failed (aligned) at offset "
                   << aligned_offset << ", size=" << aligned_size
                   << ": " << strerror(errno);
        return Status::IoError("Write failed (aligned) at offset " +
                               std::to_string(aligned_offset) +
                               ": " + strerror(errno));
    }

    return Status::OK();
}

// ---------------------------------------------------------------------------
// Read - single read with DirectIO alignment handling
// ---------------------------------------------------------------------------
Status FalconKVStore::Read(uint64_t offset, void* buffer, uint32_t size) {
    if (data_fd_ < 0) {
        LOG(ERROR) << "[FalconKVStore] Read: data file not initialized";
        return Status::IoError("Data file not initialized");
    }
    if (!buffer || size == 0) {
        LOG(ERROR) << "[FalconKVStore] Read: null buffer or zero size, offset=" << offset;
        return Status::InvalidArg("Read: null buffer or zero size");
    }

    uint32_t page_size = config_.page_size;

    bool offset_aligned = IsAligned(offset, page_size);
    bool size_aligned = IsAligned(size, page_size);
    uintptr_t buf_addr = reinterpret_cast<uintptr_t>(buffer);
    bool buf_aligned = IsAligned(buf_addr, page_size);

    if (offset_aligned && size_aligned && buf_aligned) {
        ssize_t rd = pread(data_fd_, buffer, size, static_cast<off_t>(offset));
        if (rd < 0) {
            LOG(ERROR) << "[FalconKVStore] Read failed at offset " << offset
                       << ", size=" << size << ": " << strerror(errno);
            return Status::IoError("Read failed at offset " + std::to_string(offset) +
                                   ": " + strerror(errno));
        }
        if (rd < static_cast<ssize_t>(size)) {
            LOG(ERROR) << "[FalconKVStore] Short read at offset " << offset
                       << ": expected " << size << " got " << rd;
            return Status::IoError("Short read at offset " + std::to_string(offset) +
                                   ": expected " + std::to_string(size) +
                                   " got " + std::to_string(rd));
        }
        return Status::OK();
    }

    // Slow path: need aligned buffer.
    uint64_t aligned_offset = offset & ~(static_cast<uint64_t>(page_size) - 1);
    uint64_t end = offset + size;
    uint64_t aligned_end = AlignUp(end, page_size);
    uint32_t aligned_size = static_cast<uint32_t>(aligned_end - aligned_offset);

    void* aligned_buf = AlignedAllocator::Allocate(page_size, aligned_size);
    if (!aligned_buf) {
        LOG(ERROR) << "[FalconKVStore] Read: failed to allocate aligned buffer, size="
                   << aligned_size;
        return Status::IoError("Read: failed to allocate aligned buffer");
    }

    ssize_t rd = pread(data_fd_, aligned_buf, aligned_size,
                       static_cast<off_t>(aligned_offset));
    if (rd < 0) {
        AlignedAllocator::Free(aligned_buf);
        LOG(ERROR) << "[FalconKVStore] Read failed (aligned) at offset "
                   << aligned_offset << ", size=" << aligned_size
                   << ": " << strerror(errno);
        return Status::IoError("Read failed (aligned) at offset " +
                               std::to_string(aligned_offset) +
                               ": " + strerror(errno));
    }

    // Copy the relevant portion into the user buffer.
    uint32_t copy_offset = static_cast<uint32_t>(offset - aligned_offset);
    uint32_t copy_size = std::min(size, static_cast<uint32_t>(rd) - copy_offset);
    memcpy(buffer, static_cast<uint8_t*>(aligned_buf) + copy_offset, copy_size);
    AlignedAllocator::Free(aligned_buf);

    if (copy_size < size) {
        LOG(ERROR) << "[FalconKVStore] Short read (aligned) at offset " << offset
                   << ": expected " << size << " got " << copy_size;
        return Status::IoError("Short read at offset " + std::to_string(offset) +
                               ": expected " + std::to_string(size) +
                               " got " + std::to_string(copy_size));
    }

    return Status::OK();
}

// ---------------------------------------------------------------------------
// BatchWrite - sort by offset, perform sequential writes
// ---------------------------------------------------------------------------
Status FalconKVStore::BatchWrite(const std::vector<WriteItem>& items) {
    if (items.empty()) {
        return Status::OK();
    }

    // Sort items by offset to enable sequential IO.
    std::vector<size_t> indices(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        indices[i] = i;
    }
    std::sort(indices.begin(), indices.end(), [&items](size_t a, size_t b) {
        return items[a].offset < items[b].offset;
    });

    // Write items sequentially.
    Status last_error = Status::OK();
    uint32_t fail_count = 0;
    for (size_t idx : indices) {
        const auto& item = items[idx];
        Status s = Write(item.offset, item.data, item.size);
        if (!s.ok()) {
            LOG(ERROR) << "[FalconKVStore] BatchWrite: write failed at offset "
                       << item.offset << ": " << s.ToString();
            last_error = s;
            ++fail_count;
        }
    }

    if (fail_count > 0) {
        return last_error;
    }
    return Status::OK();
}

// ---------------------------------------------------------------------------
// BatchRead - parallel reads using std::async
// ---------------------------------------------------------------------------
Status FalconKVStore::BatchRead(const std::vector<ReadItem>& items) {
    if (items.empty()) {
        return Status::OK();
    }

    // Launch async reads. We limit concurrency to io_threads.
    size_t num_threads = std::min(static_cast<size_t>(config_.io_threads), items.size());

    // For small batches, just run sequentially.
    if (items.size() <= 2 || num_threads <= 1) {
        for (const auto& item : items) {
            Status s = Read(item.offset, item.buffer, item.size);
            if (!s.ok()) {
                return s;
            }
        }
        return Status::OK();
    }

    // Split items among workers.
    std::vector<std::future<Status>> futures;
    futures.reserve(num_threads);

    size_t items_per_thread = (items.size() + num_threads - 1) / num_threads;

    for (size_t t = 0; t < num_threads; ++t) {
        size_t start = t * items_per_thread;
        size_t end = std::min(start + items_per_thread, items.size());
        if (start >= end) break;

        futures.push_back(std::async(std::launch::async, [this, &items, start, end]() {
            for (size_t i = start; i < end; ++i) {
                Status s = Read(items[i].offset, items[i].buffer, items[i].size);
                if (!s.ok()) {
                    return s;
                }
            }
            return Status::OK();
        }));
    }

    // Collect results.
    for (auto& f : futures) {
        Status s = f.get();
        if (!s.ok()) {
            return s;
        }
    }

    return Status::OK();
}

// ---------------------------------------------------------------------------
// Key-aware API: Put
// ---------------------------------------------------------------------------
StorePutResult FalconKVStore::Put(const std::string& key, const void* data,
                                   uint32_t size) {
    StorePutResult result;

    // 1. Allocate space via BuddyAllocator
    int64_t offset = allocator_->AllocChunk();
    if (offset < 0) {
        LOG(WARNING) << "[FalconKVStore] Put: out of space for key: " << key;
        result.status = Status::NoSpace("Store out of space for key: " + key);
        return result;
    }

    result.offset = static_cast<uint64_t>(offset);
    result.chunk_size = config_.chunk_size;

    // 2. Write data to SSD
    Status write_status = Write(static_cast<uint64_t>(offset), data, size);
    if (!write_status.ok()) {
        allocator_->FreeChunk(offset);
        LOG(ERROR) << "[FalconKVStore] Put: write failed for key: " << key
                   << " at offset " << offset << ": " << write_status.ToString();
        result.status = write_status;
        return result;
    }

    // 3. Record in local metadata index (committed immediately)
    StoreKeyRecord record;
    record.key = key;
    record.offset = result.offset;
    record.size = size;
    record.chunk_size = config_.chunk_size;
    record.stat = 1; // committed
    meta_index_->Put(key, record);

    // 4. Sync to Meta (best effort, non-blocking for now)
    if (meta_sync_client_) {
        meta_sync_client_->SyncCommit(store_id_, {record});
    }

    result.status = Status::OK();
    return result;
}

// ---------------------------------------------------------------------------
// Key-aware API: BatchPut
// ---------------------------------------------------------------------------
std::vector<StorePutResult> FalconKVStore::BatchPut(
    const std::vector<std::string>& keys,
    const std::vector<const void*>& data_ptrs,
    const std::vector<uint32_t>& sizes) {

    std::vector<StorePutResult> results(keys.size());

    if (keys.size() != data_ptrs.size() || keys.size() != sizes.size()) {
        LOG(ERROR) << "[FalconKVStore] BatchPut: size mismatch, keys=" << keys.size()
                   << " data_ptrs=" << data_ptrs.size() << " sizes=" << sizes.size();
        for (size_t i = 0; i < keys.size(); ++i) {
            results[i].status = Status::InvalidArg("keys/data_ptrs/sizes size mismatch");
        }
        return results;
    }

    std::vector<StoreKeyRecord> committed_records;

    for (size_t i = 0; i < keys.size(); ++i) {
        auto& result = results[i];

        // 1. Allocate space
        int64_t offset = allocator_->AllocChunk();
        if (offset < 0) {
            LOG(WARNING) << "[FalconKVStore] BatchPut: out of space for key: " << keys[i];
            result.status = Status::NoSpace("Store out of space for key: " + keys[i]);
            continue;
        }

        result.offset = static_cast<uint64_t>(offset);
        result.chunk_size = config_.chunk_size;

        // 2. Write data
        Status write_status = Write(static_cast<uint64_t>(offset),
                                    data_ptrs[i], sizes[i]);
        if (!write_status.ok()) {
            allocator_->FreeChunk(offset);
            LOG(ERROR) << "[FalconKVStore] BatchPut: write failed for key: " << keys[i]
                       << " at offset " << offset << ": " << write_status.ToString();
            result.status = write_status;
            continue;
        }

        // 3. Record in local metadata index
        StoreKeyRecord record;
        record.key = keys[i];
        record.offset = result.offset;
        record.size = sizes[i];
        record.chunk_size = config_.chunk_size;
        record.stat = 1;
        meta_index_->Put(keys[i], record);
        committed_records.push_back(record);

        result.status = Status::OK();
    }

    // 4. Batch sync to Meta
    if (meta_sync_client_ && !committed_records.empty()) {
        meta_sync_client_->SyncCommit(store_id_, committed_records);
    }

    return results;
}

// ---------------------------------------------------------------------------
// Key-aware API: Get
// ---------------------------------------------------------------------------
StoreGetResult FalconKVStore::Get(const std::string& key, void* buffer,
                                   uint32_t buffer_size) {
    StoreGetResult result;

    // 1. Look up in local metadata index
    auto record = meta_index_->Get(key);
    if (!record.has_value()) {
        result.status = Status::NotFound("Key not found in store: " + key);
        return result;
    }

    if (buffer_size < record->size) {
        result.status = Status::InvalidArg("Buffer too small for key: " + key);
        return result;
    }

    // 2. Read from SSD
    Status read_status = Read(record->offset, buffer, record->size);
    if (!read_status.ok()) {
        result.status = read_status;
        return result;
    }

    result.size = record->size;
    result.status = Status::OK();

    // 3. Update access time
    meta_index_->Touch(key);

    return result;
}

// ---------------------------------------------------------------------------
// Key-aware API: Contains
// ---------------------------------------------------------------------------
bool FalconKVStore::Contains(const std::string& key) {
    return meta_index_->Get(key).has_value();
}

// ---------------------------------------------------------------------------
// Key-aware API: BatchContains
// ---------------------------------------------------------------------------
void FalconKVStore::BatchContains(const std::vector<std::string>& keys,
                                    std::vector<StoreKeyRecord>& hits,
                                    std::vector<std::string>& misses) {
    meta_index_->BatchContains(keys, hits, misses);
}

// ---------------------------------------------------------------------------
// Key-aware API: Remove
// ---------------------------------------------------------------------------
Status FalconKVStore::Remove(const std::string& key) {
    // 1. Sync removal to Meta first (wait for confirmation).
    if (meta_sync_client_) {
        Status s = meta_sync_client_->SyncRemove(store_id_, {key});
        if (!s.ok()) {
            LOG(ERROR) << "[FalconKVStore] Remove: SyncRemove failed for key: " << key
                       << ": " << s.ToString();
            return s;
        }
    }

    // 2. Remove from local index.
    auto record = meta_index_->Remove(key);
    if (!record.has_value()) {
        LOG(WARNING) << "[FalconKVStore] Remove: key not found in local index: " << key;
        return Status::NotFound("Key not found for removal: " + key);
    }

    // 3. Enqueue for deferred space reclamation (grace period).
    pending_evict_queue_->Enqueue(key, record->offset);

    return Status::OK();
}

// ---------------------------------------------------------------------------
// Key-aware API: GetUsageRatio
// ---------------------------------------------------------------------------
double FalconKVStore::GetUsageRatio() const {
    if (!allocator_) return 0.0;
    return allocator_->GetUsageRatio();
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------
void FalconKVStore::Close() {
    if (!running_.exchange(false)) {
        return;
    }

    // Stop background threads before releasing resources they depend on.
    evict_manager_.reset();
    pending_evict_queue_.reset();

    scheduler_proxy_.reset();
    if (meta_sync_client_) {
        meta_sync_client_->StopReconnectLoop();
    }
    meta_sync_client_.reset();
    meta_index_.reset();
    allocator_.reset();
    buffer_pool_.reset();

    if (data_fd_ >= 0) {
        fsync(data_fd_);
        close(data_fd_);
        data_fd_ = -1;
    }
}

} // namespace falconkv
