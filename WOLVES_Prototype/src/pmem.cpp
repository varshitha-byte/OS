#include "pmem.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>

namespace wofs {

PMemSimulator::PMemSimulator(const std::string& backing_file, size_t size)
    : backing_file_(backing_file), next_free_offset_(SUPERBLOCK_SIZE) {
    if (size < SUPERBLOCK_SIZE * 2) {
        throw std::runtime_error("Simulated PMEM size must be at least 8 KiB!");
    }
    memory_.resize(size, 0);

    // Check if backing file exists
    std::ifstream check(backing_file_, std::ios::binary);
    if (check) {
        load_from_disk();
    } else {
        init_superblock();
        save_to_disk();
    }
}

PMemSimulator::~PMemSimulator() {
    std::lock_guard<std::mutex> lock(pmem_mutex_);
    save_to_disk();
}

void PMemSimulator::init_superblock() {
    // Layout in Block 0 (Offset 0..4095):
    // [0..3]:   Magic 'WOSB' (0x574F5342)
    // [4..7]:   Group Size (4096)
    // [8..11]:  Total Groups (size / 4096)
    // [12..15]: Bitmap Size in bytes
    // [16..]:   Coarse Persistence Bitmap bytes
    uint32_t magic = SUPERBLOCK_MAGIC;
    uint32_t group_size = static_cast<uint32_t>(PKG_GROUP_SIZE);
    uint32_t total_groups = static_cast<uint32_t>(memory_.size() / PKG_GROUP_SIZE);
    uint32_t bitmap_bytes = (total_groups + 7) / 8;

    std::memcpy(memory_.data() + 0, &magic, 4);
    std::memcpy(memory_.data() + 4, &group_size, 4);
    std::memcpy(memory_.data() + 8, &total_groups, 4);
    std::memcpy(memory_.data() + 12, &bitmap_bytes, 4);

    // Group 0 (Superblock) is always active
    memory_[16] = 0x01;
}

uint64_t PMemSimulator::allocate(size_t size) {
    std::lock_guard<std::mutex> lock(pmem_mutex_);

    // Align allocations to 8 bytes
    if (next_free_offset_ % 8 != 0) {
        next_free_offset_ += 8 - (next_free_offset_ % 8);
    }

    // Paper §4.4: Packages must never straddle a 4 KiB Package Group boundary
    if (size <= PKG_GROUP_SIZE) {
        size_t grp_start = next_free_offset_ / PKG_GROUP_SIZE;
        size_t grp_end = (next_free_offset_ + size - 1) / PKG_GROUP_SIZE;
        if (grp_start != grp_end) {
            next_free_offset_ = grp_end * PKG_GROUP_SIZE;
        }
    }
    
    if (next_free_offset_ + size > memory_.size()) {
        throw std::runtime_error("Out of simulated PMEM space!");
    }

    uint64_t allocated = next_free_offset_;
    next_free_offset_ += size;

    // Track C: Mark all affected 4 KiB Package Groups as active in the Coarse Bitmap
    size_t start_grp = allocated / PKG_GROUP_SIZE;
    size_t end_grp = (allocated + size - 1) / PKG_GROUP_SIZE;
    for (size_t g = start_grp; g <= end_grp; ++g) {
        mark_group_active(g);
    }

    return allocated;
}

void PMemSimulator::write(uint64_t offset, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(pmem_mutex_);
    if (offset + size > memory_.size()) {
        throw std::runtime_error("Write out of bounds!");
    }
    std::memcpy(memory_.data() + offset, data, size);

    // Ensure touched groups are marked active
    size_t start_grp = offset / PKG_GROUP_SIZE;
    size_t end_grp = (offset + size - 1) / PKG_GROUP_SIZE;
    for (size_t g = start_grp; g <= end_grp; ++g) {
        mark_group_active(g);
    }
}

void PMemSimulator::read(uint64_t offset, void* data, size_t size) const {
    std::lock_guard<std::mutex> lock(pmem_mutex_);
    if (offset + size > memory_.size()) {
        throw std::runtime_error("Read out of bounds!");
    }
    std::memcpy(data, memory_.data() + offset, size);
}

void PMemSimulator::persist(uint64_t offset, size_t size) {
    std::lock_guard<std::mutex> lock(pmem_mutex_);
    if (offset + size > memory_.size()) return;

    // Persist dirty range directly to disk (mimicking clwb + sfence)
    std::fstream out(backing_file_, std::ios::in | std::ios::out | std::ios::binary);
    if (!out) {
        save_to_disk();
        return;
    }
    out.seekp(offset);
    out.write(reinterpret_cast<const char*>(memory_.data() + offset), size);

    // Also persist any dirty bitmap updates in Superblock Block 0
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(memory_.data()), SUPERBLOCK_SIZE);
    out.flush();
}

bool PMemSimulator::is_group_active(size_t group_idx) const {
    size_t total_groups = get_total_groups();
    if (group_idx >= total_groups) return false;

    size_t byte_idx = 16 + (group_idx / 8);
    if (byte_idx >= SUPERBLOCK_SIZE) return false;

    uint8_t bit_mask = static_cast<uint8_t>(1 << (group_idx % 8));
    return (memory_[byte_idx] & bit_mask) != 0;
}

void PMemSimulator::mark_group_active(size_t group_idx) {
    size_t total_groups = get_total_groups();
    if (group_idx >= total_groups) return;

    size_t byte_idx = 16 + (group_idx / 8);
    if (byte_idx >= SUPERBLOCK_SIZE) return;

    uint8_t bit_mask = static_cast<uint8_t>(1 << (group_idx % 8));
    memory_[byte_idx] |= bit_mask;
}

size_t PMemSimulator::get_total_groups() const {
    uint32_t total = 0;
    std::memcpy(&total, memory_.data() + 8, 4);
    return (total > 0) ? total : (memory_.size() / PKG_GROUP_SIZE);
}

size_t PMemSimulator::get_active_group_count() const {
    size_t count = 0;
    size_t total = get_total_groups();
    for (size_t i = 0; i < total; ++i) {
        if (is_group_active(i)) count++;
    }
    return count;
}

void PMemSimulator::replace_memory(const std::vector<uint8_t>& new_memory, uint64_t next_free_offset) {
    std::lock_guard<std::mutex> lock(pmem_mutex_);
    if (new_memory.size() != memory_.size()) {
        throw std::runtime_error("Compacted memory buffer size mismatch!");
    }
    memory_ = new_memory;
    init_superblock();
    next_free_offset_ = std::max<uint64_t>(SUPERBLOCK_SIZE, next_free_offset);

    // Mark groups active for the compacted data
    size_t end_grp = (next_free_offset_ + PKG_GROUP_SIZE - 1) / PKG_GROUP_SIZE;
    for (size_t g = 0; g <= end_grp; ++g) {
        mark_group_active(g);
    }
    save_to_disk();
}

uint64_t PMemSimulator::get_allocated_offset() const {
    std::lock_guard<std::mutex> lock(pmem_mutex_);
    return next_free_offset_;
}

void PMemSimulator::set_allocated_offset(uint64_t offset) {
    std::lock_guard<std::mutex> lock(pmem_mutex_);
    next_free_offset_ = std::max<uint64_t>(SUPERBLOCK_SIZE, offset);
}

void PMemSimulator::load_from_disk() {
    std::ifstream in(backing_file_, std::ios::binary | std::ios::ate);
    if (in) {
        std::streamsize file_len = in.tellg();
        in.seekg(0, std::ios::beg);
        size_t bytes_to_read = std::min<size_t>(file_len, memory_.size());
        in.read(reinterpret_cast<char*>(memory_.data()), bytes_to_read);

        uint32_t magic = 0;
        std::memcpy(&magic, memory_.data(), 4);
        if (magic != SUPERBLOCK_MAGIC) {
            init_superblock();
        }
    } else {
        init_superblock();
    }
}

void PMemSimulator::save_to_disk() {
    std::ofstream out(backing_file_, std::ios::binary | std::ios::trunc);
    if (out) {
        out.write(reinterpret_cast<const char*>(memory_.data()), memory_.size());
    }
}

} // namespace wofs
