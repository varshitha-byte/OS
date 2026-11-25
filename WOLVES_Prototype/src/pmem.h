#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

namespace wofs {

constexpr size_t PKG_GROUP_SIZE    = 4096; // 4 KiB Package Group (§4.4)
constexpr size_t SUPERBLOCK_SIZE   = 4096; // 4 KiB reserved for Superblock & Bitmap
constexpr uint32_t SUPERBLOCK_MAGIC = 0x574F5342; // 'WOSB'

class PMemSimulator {
public:
    PMemSimulator(const std::string& backing_file, size_t size);
    ~PMemSimulator();

    // Allocate continuous space in PM (8-byte aligned, starts after Superblock)
    uint64_t allocate(size_t size);

    // Thread-safe read/write/persist
    void write(uint64_t offset, const void* data, size_t size);
    void read(uint64_t offset, void* data, size_t size) const;
    void persist(uint64_t offset, size_t size);

    // Replaces memory content with compacted data (used by Garbage Collector)
    void replace_memory(const std::vector<uint8_t>& new_memory, uint64_t next_free_offset);

    // Track C: Coarse Persistence Bitmap Operations (§4.4 & §5.1)
    bool is_group_active(size_t group_idx) const;
    void mark_group_active(size_t group_idx);
    size_t get_total_groups() const;
    size_t get_active_group_count() const;

    // Direct access & sizing
    uint8_t* get_base() { return memory_.data(); }
    const uint8_t* get_base() const { return memory_.data(); }
    size_t get_size() const { return memory_.size(); }
    uint64_t get_allocated_offset() const;
    void set_allocated_offset(uint64_t offset);

private:
    std::string backing_file_;
    std::vector<uint8_t> memory_;
    uint64_t next_free_offset_;
    mutable std::mutex pmem_mutex_; // Thread safety (Track F)

    void init_superblock();
    void load_from_disk();
    void save_to_disk();
};

} // namespace wofs
