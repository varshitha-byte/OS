#include "wofs.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace wofs {

WOLVESFileSystem::WOLVESFileSystem(std::shared_ptr<PMemSimulator> pmem)
    : pmem_(pmem), next_inode_id_(1), current_seq_num_(1) {
}

void WOLVESFileSystem::mount() {
    std::cout << "Mounting WOLVES File System...\n";
    package_translation_layer();
    std::cout << "Mounted successfully. Found " << inodes_.size() << " valid files.\n";
}

void WOLVESFileSystem::package_translation_layer() {
    inodes_.clear();
    uint64_t offset = 0;
    size_t pmem_size = pmem_->get_size();
    
    uint32_t max_inode = 0;
    uint32_t max_seq = 0;

    // Scan PM to find and translate packages
    while (offset + sizeof(Package) <= pmem_size) {
        Package pkg;
        pmem_->read(offset, &pkg, sizeof(Package));

        // Check if we hit uninitialized space
        if (pkg.magic != PACKAGE_MAGIC) {
            offset += 8; // Step forward and search
            continue;
        }

        // Verify Checksum
        uint32_t expected_csum = calculate_checksum(pkg);
        if (pkg.checksum != expected_csum) {
            std::cerr << "Warning: Corrupted package at offset " << offset << "\n";
            offset += sizeof(Package);
            continue;
        }

        // Apply package to in-memory state
        if (pkg.op == OpType::CREATE_FILE) {
            Inode inode;
            inode.id = pkg.inode_id;
            inode.name = pkg.filename;
            inode.size = 0;
            inodes_[inode.id] = inode;
        } 
        else if (pkg.op == OpType::WRITE_DATA) {
            if (inodes_.find(pkg.inode_id) != inodes_.end()) {
                Inode& inode = inodes_[pkg.inode_id];
                inode.extents.push_back({pkg.offset, pkg.size, pkg.data_pmem_ptr});
                inode.size = std::max<size_t>(inode.size, pkg.offset + pkg.size);
            }
        }
        else if (pkg.op == OpType::DELETE_FILE) {
            inodes_.erase(pkg.inode_id);
        }

        if (pkg.inode_id > max_inode) max_inode = pkg.inode_id;
        if (pkg.seq_num > max_seq) max_seq = pkg.seq_num;

        offset += sizeof(Package);
    }

    next_inode_id_ = max_inode + 1;
    current_seq_num_ = max_seq + 1;
}

void WOLVESFileSystem::append_package(const Package& pkg) {
    uint64_t offset = pmem_->allocate(sizeof(Package));
    pmem_->write(offset, &pkg, sizeof(Package));
    pmem_->persist(offset, sizeof(Package));
}

uint32_t WOLVESFileSystem::create_file(const std::string& name) {
    Package pkg = {};
    pkg.magic = PACKAGE_MAGIC;
    pkg.seq_num = current_seq_num_++;
    pkg.op = OpType::CREATE_FILE;
    pkg.inode_id = next_inode_id_++;
    std::strncpy(pkg.filename, name.c_str(), sizeof(pkg.filename) - 1);
    
    pkg.checksum = calculate_checksum(pkg);

    // Single ordering point: append the package to PM
    append_package(pkg);

    // Update in-memory state
    Inode inode;
    inode.id = pkg.inode_id;
    inode.name = name;
    inode.size = 0;
    inodes_[inode.id] = inode;

    return pkg.inode_id;
}

void WOLVESFileSystem::write_file(uint32_t inode_id, uint32_t file_offset, const void* data, uint32_t size) {
    if (inodes_.find(inode_id) == inodes_.end()) {
        throw std::runtime_error("File does not exist!");
    }

    // 1. Write data to PM first (Data Journaling equivalent, but orderless before the package)
    uint64_t data_offset = pmem_->allocate(size);
    pmem_->write(data_offset, data, size);
    pmem_->persist(data_offset, size);

    // 2. Write the Write-Once Package
    Package pkg = {};
    pkg.magic = PACKAGE_MAGIC;
    pkg.seq_num = current_seq_num_++;
    pkg.op = OpType::WRITE_DATA;
    pkg.inode_id = inode_id;
    pkg.offset = file_offset;
    pkg.size = size;
    pkg.data_pmem_ptr = data_offset;
    
    pkg.checksum = calculate_checksum(pkg);
    append_package(pkg);

    // Update in-memory state
    Inode& inode = inodes_[inode_id];
    inode.extents.push_back({file_offset, size, data_offset});
    inode.size = std::max<size_t>(inode.size, file_offset + size);
}

void WOLVESFileSystem::read_file(uint32_t inode_id, uint32_t file_offset, void* data, uint32_t size) {
    if (inodes_.find(inode_id) == inodes_.end()) {
        throw std::runtime_error("File does not exist!");
    }

    Inode& inode = inodes_[inode_id];
    if (file_offset + size > inode.size) {
        throw std::runtime_error("Read out of bounds!");
    }

    // Simple extent matching (ignores overlapping extents for simplicity in prototype)
    uint8_t* out = reinterpret_cast<uint8_t*>(data);
    uint32_t bytes_read = 0;

    // Scan backwards to find the most recent write to this region
    // For a robust implementation, we'd need a proper interval tree.
    for (auto it = inode.extents.rbegin(); it != inode.extents.rend(); ++it) {
        const auto& ext = *it;
        if (file_offset >= ext.offset && file_offset < ext.offset + ext.size) {
            uint32_t read_start = file_offset - ext.offset;
            uint32_t read_size = std::min(size - bytes_read, ext.size - read_start);
            
            pmem_->read(ext.pmem_offset + read_start, out + bytes_read, read_size);
            
            bytes_read += read_size;
            file_offset += read_size;
            
            if (bytes_read == size) break;
        }
    }

    if (bytes_read < size) {
        // Zero out uninitialized parts
        std::memset(out + bytes_read, 0, size - bytes_read);
    }
}

void WOLVESFileSystem::delete_file(uint32_t inode_id) {
    if (inodes_.find(inode_id) == inodes_.end()) {
        return;
    }

    Package pkg = {};
    pkg.magic = PACKAGE_MAGIC;
    pkg.seq_num = current_seq_num_++;
    pkg.op = OpType::DELETE_FILE;
    pkg.inode_id = inode_id;
    
    pkg.checksum = calculate_checksum(pkg);
    append_package(pkg);

    inodes_.erase(inode_id);
}

void WOLVESFileSystem::list_files() const {
    std::cout << "--- Files in WOLVES ---\n";
    for (const auto& pair : inodes_) {
        std::cout << "Inode: " << pair.first << " | Name: " << pair.second.name 
                  << " | Size: " << pair.second.size << " bytes\n";
    }
    std::cout << "-----------------------\n";
}

uint32_t WOLVESFileSystem::find_file(const std::string& path) const {
    // Path comes from FUSE, e.g., "/document.txt"
    // Our simulator stored them just as "document.txt" or whatever name was passed.
    // So if path starts with '/', we strip it.
    std::string search_name = path;
    if (!search_name.empty() && search_name[0] == '/') {
        search_name = search_name.substr(1);
    }
    
    for (const auto& pair : inodes_) {
        if (pair.second.name == search_name) {
            return pair.first;
        }
    }
    return 0;
}

size_t WOLVESFileSystem::get_file_size(uint32_t inode_id) const {
    auto it = inodes_.find(inode_id);
    if (it != inodes_.end()) {
        return it->second.size;
    }
    return 0;
}

std::vector<std::string> WOLVESFileSystem::get_all_filenames() const {
    std::vector<std::string> names;
    for (const auto& pair : inodes_) {
        names.push_back(pair.second.name);
    }
    return names;
}

} // namespace wofs
