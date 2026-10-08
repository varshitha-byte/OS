#pragma once
#include "pmem.h"
#include "package.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace wofs {

// In-memory representation of a file, built by the Package Translation Layer
struct Inode {
    uint32_t id;
    std::string name;
    size_t size;
    
    // Ordered list of data extents
    struct Extent {
        uint32_t offset;
        uint32_t size;
        uint64_t pmem_offset;
    };
    std::vector<Extent> extents;
};

class WOLVESFileSystem {
public:
    WOLVESFileSystem(std::shared_ptr<PMemSimulator> pmem);

    // Mounts the file system and runs the Package Translation Layer
    void mount();

    // File Operations
    uint32_t create_file(const std::string& name);
    void write_file(uint32_t inode_id, uint32_t offset, const void* data, uint32_t size);
    void read_file(uint32_t inode_id, uint32_t offset, void* data, uint32_t size);
    void delete_file(uint32_t inode_id);

    // List files (for debugging)
    void list_files() const;

    // --- FUSE Helpers ---
    // Returns 0 if not found, otherwise returns inode_id
    uint32_t find_file(const std::string& path) const;
    
    // Get file size
    size_t get_file_size(uint32_t inode_id) const;

    // Get all files (for readdir)
    std::vector<std::string> get_all_filenames() const;

private:
    std::shared_ptr<PMemSimulator> pmem_;
    
    // In-memory VFS structures
    std::unordered_map<uint32_t, Inode> inodes_;
    uint32_t next_inode_id_;
    uint32_t current_seq_num_;

    // Writes a package to PM, ensuring it's durable in one ordering point
    void append_package(const Package& pkg);
    
    // Recovers the FS state from packages
    void package_translation_layer();
};

} // namespace wofs
