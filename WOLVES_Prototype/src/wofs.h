#pragma once

#include "pmem.h"
#include "package.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <shared_mutex>
#include <atomic>

namespace wofs {

// Contiguous range of data belonging to a file
struct Extent {
    uint32_t offset;       // File offset
    uint32_t size;         // Size of this extent
    uint64_t pmem_offset;  // PMEM location
};

// In-memory Virtual File System (VFS) representation
struct Inode {
    uint32_t id;
    uint32_t parent_id;
    bool is_directory;
    std::string name;
    size_t size;
    std::vector<Extent> extents;
    std::vector<uint32_t> children_inodes; // Child inodes for directories
};

// Snapshot representation (Track A)
struct SnapshotInfo {
    std::string name;
    uint32_t seq_num;
    uint32_t active_nodes_count;
};

// Statistics gathered by the Garbage Collector / Analyzer
struct GCStats {
    size_t total_pmem_bytes;
    size_t allocated_bytes;
    size_t live_bytes;
    size_t dead_bytes;
    size_t total_packages;
    size_t live_packages;
    size_t dead_packages;
    size_t active_4k_groups; // Track C metric
};

class WOLVESFileSystem {
public:
    explicit WOLVESFileSystem(std::shared_ptr<PMemSimulator> pmem);

    // Mounts file system and triggers PTL recovery
    void mount();

    // Core POSIX-like File Operations (Thread-safe via Track F)
    uint32_t create_file(const std::string& path);
    uint32_t create_directory(const std::string& path);
    void write_file(uint32_t inode_id, uint32_t file_offset, const void* data, uint32_t size);
    void read_file(uint32_t inode_id, uint32_t file_offset, void* data, uint32_t size) const;
    void delete_file(uint32_t inode_id);
    void delete_directory(uint32_t inode_id);

    // Atomic Compound Rename
    void rename(const std::string& old_path, const std::string& new_path);

    // Track A: Instant Time-Travel & Snapshots
    void create_snapshot(const std::string& snapshot_name);
    std::vector<SnapshotInfo> list_snapshots() const;
    void restore_snapshot(const std::string& snapshot_name);
    void rollback_to_seq(uint32_t target_seq);

    // Track C & GC: Compaction & Coarse Persistence Analysis
    GCStats get_gc_stats() const;
    void compact();

    // Path resolution and metadata lookup (Thread-safe shared read locks)
    uint32_t resolve_path(const std::string& path) const;
    const Inode* get_inode(uint32_t inode_id) const;
    size_t get_file_size(uint32_t inode_id) const;
    bool is_directory(uint32_t inode_id) const;
    std::vector<std::string> list_directory(uint32_t dir_inode_id) const;
    void print_tree(uint32_t dir_id = 1, int indent = 0) const;

private:
    std::shared_ptr<PMemSimulator> pmem_;
    std::unordered_map<uint32_t, Inode> inodes_;
    std::vector<SnapshotInfo> snapshots_;
    
    // Concurrency controls (Track F)
    mutable std::shared_mutex vfs_mutex_;
    std::atomic<uint32_t> next_inode_id_;
    std::atomic<uint32_t> current_seq_num_;

    // Private unlocked helpers to avoid recursive deadlocks on std::shared_mutex
    uint32_t resolve_path_unlocked(const std::string& path) const;
    void read_file_unlocked(uint32_t inode_id, uint32_t file_offset, void* data, uint32_t size) const;
    GCStats get_gc_stats_unlocked() const;

    // Package Translation Layer (PTL) with Coarse Bitmap acceleration (Track C)
    void package_translation_layer(uint32_t max_seq_threshold = UINT32_MAX);

    // Appends a single write-once package to PMEM with persist ordering
    uint64_t append_package(const Package& pkg);

    // Path parsing helpers
    static void split_path(const std::string& path, std::string& parent_path, std::string& base_name);
    static std::vector<std::string> tokenize_path(const std::string& path);
};

} // namespace wofs
