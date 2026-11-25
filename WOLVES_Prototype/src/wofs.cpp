#include "wofs.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace wofs {

WOLVESFileSystem::WOLVESFileSystem(std::shared_ptr<PMemSimulator> pmem)
    : pmem_(pmem), next_inode_id_(2), current_seq_num_(1) {
}

void WOLVESFileSystem::mount() {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);
    std::cout << "[WOLVES] Mounting File System with Coarse Persistence Acceleration...\n";
    package_translation_layer(UINT32_MAX);
    std::cout << "[WOLVES] Mounted successfully. In-Memory VFS reconstructed: " 
              << inodes_.size() << " nodes, " << snapshots_.size() << " snapshots.\n";
}

std::vector<std::string> WOLVESFileSystem::tokenize_path(const std::string& path) {
    std::vector<std::string> tokens;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '/')) {
        if (!item.empty()) {
            tokens.push_back(item);
        }
    }
    return tokens;
}

void WOLVESFileSystem::split_path(const std::string& path, std::string& parent_path, std::string& base_name) {
    auto tokens = tokenize_path(path);
    if (tokens.empty()) {
        parent_path = "/";
        base_name = "";
        return;
    }
    if (tokens.size() == 1) {
        parent_path = "/";
        base_name = tokens[0];
        return;
    }
    base_name = tokens.back();
    parent_path = "";
    for (size_t i = 0; i < tokens.size() - 1; ++i) {
        parent_path += "/" + tokens[i];
    }
}

uint32_t WOLVESFileSystem::resolve_path_unlocked(const std::string& path) const {
    auto tokens = tokenize_path(path);
    if (tokens.empty()) {
        return 1; // Root directory
    }

    uint32_t curr = 1;
    for (const auto& token : tokens) {
        auto it = inodes_.find(curr);
        if (it == inodes_.end() || !it->second.is_directory) {
            return 0;
        }

        uint32_t match = 0;
        for (uint32_t child_id : it->second.children_inodes) {
            auto child_it = inodes_.find(child_id);
            if (child_it != inodes_.end() && child_it->second.name == token) {
                match = child_id;
                break;
            }
        }
        if (match == 0) {
            return 0;
        }
        curr = match;
    }
    return curr;
}

uint32_t WOLVESFileSystem::resolve_path(const std::string& path) const {
    std::shared_lock<std::shared_mutex> lock(vfs_mutex_);
    return resolve_path_unlocked(path);
}

void WOLVESFileSystem::package_translation_layer(uint32_t max_seq_threshold) {
    inodes_.clear();
    snapshots_.clear();

    // Inode 1 is always the root directory
    Inode root;
    root.id = 1;
    root.parent_id = 1;
    root.is_directory = true;
    root.name = "/";
    root.size = 0;
    inodes_[1] = root;

    uint32_t max_inode = 1;
    uint32_t max_seq = 0;
    uint64_t highest_pmem_offset = SUPERBLOCK_SIZE;

    size_t total_groups = pmem_->get_total_groups();

    // Track C: Coarse Persistence Scanning (§4.4 & §5.1)
    // Read the Coarse Persistence Bitmap to only inspect active 4 KiB Package Groups
    for (size_t g = 1; g < total_groups; ++g) {
        if (!pmem_->is_group_active(g)) {
            continue;
        }

        uint64_t group_start = g * PKG_GROUP_SIZE;
        uint64_t group_end = std::min<uint64_t>(group_start + PKG_GROUP_SIZE, pmem_->get_size());
        uint64_t offset = group_start;

        while (offset + sizeof(Package) <= group_end) {
            Package pkg;
            pmem_->read(offset, &pkg, sizeof(Package));

            if (pkg.magic != PACKAGE_MAGIC) {
                offset += 8;
                continue;
            }

            // Verify Checksum
            uint32_t expected_csum = calculate_checksum(pkg);
            if (pkg.checksum != expected_csum) {
                std::cerr << "[PTL] Warning: Corrupted package at PM offset 0x" 
                          << std::hex << offset << std::dec << " — safely ignoring.\n";
                offset += sizeof(Package);
                continue;
            }

            highest_pmem_offset = std::max(highest_pmem_offset, offset + sizeof(Package));

            // Track A: Time-Travel filtering
            if (pkg.seq_num > max_seq_threshold) {
                offset += sizeof(Package);
                continue;
            }

            OpType op = static_cast<OpType>(pkg.op);
            uint32_t parent = (pkg.parent_inode_id == 0) ? 1 : pkg.parent_inode_id;

            if (op == OpType::CREATE_FILE || op == OpType::CREATE_DIR) {
                Inode inode;
                inode.id = pkg.inode_id;
                inode.parent_id = parent;
                inode.is_directory = (op == OpType::CREATE_DIR);
                inode.name = pkg.filename;
                inode.size = 0;
                inodes_[inode.id] = inode;
            } 
            else if (op == OpType::WRITE_DATA) {
                auto it = inodes_.find(pkg.inode_id);
                if (it != inodes_.end() && !it->second.is_directory) {
                    it->second.extents.push_back({pkg.offset, pkg.size, pkg.data_pmem_ptr});
                    it->second.size = std::max<size_t>(it->second.size, pkg.offset + pkg.size);
                    highest_pmem_offset = std::max(highest_pmem_offset, pkg.data_pmem_ptr + pkg.size);
                }
            } 
            else if (op == OpType::DELETE_FILE) {
                inodes_.erase(pkg.inode_id);
            }
            else if (op == OpType::RENAME) {
                bool compound_valid = true;
                if ((pkg.flags & PKG_FLAG_COMPOUND) && pkg.fwd_ptr != 0) {
                    Package linked_pkg;
                    pmem_->read(pkg.fwd_ptr, &linked_pkg, sizeof(Package));
                    if (linked_pkg.magic != PACKAGE_MAGIC || linked_pkg.checksum != calculate_checksum(linked_pkg)) {
                        compound_valid = false;
                        std::cerr << "[PTL] Warning: Compound forward link invalid. Rolling back rename.\n";
                    }
                }

                if (compound_valid) {
                    auto it = inodes_.find(pkg.inode_id);
                    if (it != inodes_.end()) {
                        it->second.parent_id = parent;
                        it->second.name = pkg.filename;
                    }
                }
            }
            else if (op == OpType::SNAPSHOT) {
                snapshots_.push_back({pkg.filename, pkg.offset, static_cast<uint32_t>(inodes_.size())});
                std::cout << "[PTL] Registered Snapshot: \"" << pkg.filename 
                          << "\" (Seq=" << pkg.offset << ")\n";
            }

            if (pkg.inode_id > max_inode) max_inode = pkg.inode_id;
            if (pkg.seq_num > max_seq) max_seq = pkg.seq_num;

            offset += sizeof(Package);
        }
    }

    // Reconstruct directory child hierarchy
    for (const auto& pair : inodes_) {
        uint32_t id = pair.first;
        if (id == 1) continue; // Skip root

        uint32_t p_id = pair.second.parent_id;
        auto p_it = inodes_.find(p_id);
        if (p_it != inodes_.end() && p_it->second.is_directory) {
            p_it->second.children_inodes.push_back(id);
        }
    }

    if (highest_pmem_offset % 8 != 0) {
        highest_pmem_offset += 8 - (highest_pmem_offset % 8);
    }
    pmem_->set_allocated_offset(highest_pmem_offset);

    next_inode_id_.store(max_inode + 1);
    current_seq_num_.store(max_seq + 1);
}

uint64_t WOLVESFileSystem::append_package(const Package& pkg) {
    uint64_t offset = pmem_->allocate(sizeof(Package));
    pmem_->write(offset, &pkg, sizeof(Package));
    pmem_->persist(offset, sizeof(Package));
    return offset;
}

uint32_t WOLVESFileSystem::create_directory(const std::string& path) {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);

    std::string parent_path, dir_name;
    split_path(path, parent_path, dir_name);

    if (dir_name.empty()) {
        throw std::runtime_error("Invalid directory name!");
    }

    uint32_t parent_id = resolve_path_unlocked(parent_path);
    if (parent_id == 0 || !inodes_[parent_id].is_directory) {
        throw std::runtime_error("Parent directory does not exist: " + parent_path);
    }

    for (uint32_t child_id : inodes_[parent_id].children_inodes) {
        if (inodes_[child_id].name == dir_name) {
            throw std::runtime_error("Directory or file already exists: " + dir_name);
        }
    }

    uint32_t new_id = next_inode_id_++;

    Package pkg = {};
    pkg.magic = PACKAGE_MAGIC;
    pkg.seq_num = current_seq_num_++;
    pkg.op = static_cast<uint8_t>(OpType::CREATE_DIR);
    pkg.flags = PKG_FLAG_NONE;
    pkg.inode_id = new_id;
    pkg.parent_inode_id = parent_id;
    std::strncpy(pkg.filename, dir_name.c_str(), sizeof(pkg.filename) - 1);
    pkg.checksum = calculate_checksum(pkg);

    append_package(pkg);

    Inode inode;
    inode.id = new_id;
    inode.parent_id = parent_id;
    inode.is_directory = true;
    inode.name = dir_name;
    inode.size = 0;
    inodes_[new_id] = inode;
    inodes_[parent_id].children_inodes.push_back(new_id);

    return new_id;
}

uint32_t WOLVESFileSystem::create_file(const std::string& path) {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);

    std::string parent_path, file_name;
    split_path(path, parent_path, file_name);

    if (file_name.empty()) {
        throw std::runtime_error("Invalid file name!");
    }

    uint32_t parent_id = resolve_path_unlocked(parent_path);
    if (parent_id == 0 || !inodes_[parent_id].is_directory) {
        throw std::runtime_error("Parent directory does not exist: " + parent_path);
    }

    for (uint32_t child_id : inodes_[parent_id].children_inodes) {
        if (inodes_[child_id].name == file_name) {
            return child_id; // Already exists
        }
    }

    uint32_t new_id = next_inode_id_++;

    Package pkg = {};
    pkg.magic = PACKAGE_MAGIC;
    pkg.seq_num = current_seq_num_++;
    pkg.op = static_cast<uint8_t>(OpType::CREATE_FILE);
    pkg.flags = PKG_FLAG_NONE;
    pkg.inode_id = new_id;
    pkg.parent_inode_id = parent_id;
    std::strncpy(pkg.filename, file_name.c_str(), sizeof(pkg.filename) - 1);
    pkg.checksum = calculate_checksum(pkg);

    append_package(pkg);

    Inode inode;
    inode.id = new_id;
    inode.parent_id = parent_id;
    inode.is_directory = false;
    inode.name = file_name;
    inode.size = 0;
    inodes_[new_id] = inode;
    inodes_[parent_id].children_inodes.push_back(new_id);

    return new_id;
}

void WOLVESFileSystem::write_file(uint32_t inode_id, uint32_t file_offset, const void* data, uint32_t size) {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);

    auto it = inodes_.find(inode_id);
    if (it == inodes_.end() || it->second.is_directory) {
        throw std::runtime_error("Target inode is not a valid regular file!");
    }

    uint64_t data_offset = pmem_->allocate(size);
    pmem_->write(data_offset, data, size);
    pmem_->persist(data_offset, size);

    Package pkg = {};
    pkg.magic = PACKAGE_MAGIC;
    pkg.seq_num = current_seq_num_++;
    pkg.op = static_cast<uint8_t>(OpType::WRITE_DATA);
    pkg.flags = PKG_FLAG_NONE;
    pkg.inode_id = inode_id;
    pkg.parent_inode_id = it->second.parent_id;
    pkg.offset = file_offset;
    pkg.size = size;
    pkg.data_pmem_ptr = data_offset;
    std::strncpy(pkg.filename, it->second.name.c_str(), sizeof(pkg.filename) - 1);
    pkg.checksum = calculate_checksum(pkg);

    append_package(pkg);

    it->second.extents.push_back({file_offset, size, data_offset});
    it->second.size = std::max<size_t>(it->second.size, file_offset + size);
}

void WOLVESFileSystem::read_file_unlocked(uint32_t inode_id, uint32_t file_offset, void* data, uint32_t size) const {
    auto it = inodes_.find(inode_id);
    if (it == inodes_.end() || it->second.is_directory) {
        throw std::runtime_error("Target file does not exist!");
    }

    const Inode& inode = it->second;
    uint8_t* out = reinterpret_cast<uint8_t*>(data);

    if (file_offset >= inode.size) {
        std::memset(out, 0, size);
        return;
    }

    uint32_t bytes_to_read = std::min<uint32_t>(size, inode.size - file_offset);
    uint32_t bytes_read = 0;

    for (auto ext_it = inode.extents.rbegin(); ext_it != inode.extents.rend(); ++ext_it) {
        const auto& ext = *ext_it;
        if (file_offset >= ext.offset && file_offset < ext.offset + ext.size) {
            uint32_t read_start = file_offset - ext.offset;
            uint32_t chunk_size = std::min(bytes_to_read - bytes_read, ext.size - read_start);

            pmem_->read(ext.pmem_offset + read_start, out + bytes_read, chunk_size);

            bytes_read += chunk_size;
            file_offset += chunk_size;

            if (bytes_read == bytes_to_read) break;
        }
    }

    if (size > bytes_read) {
        std::memset(out + bytes_read, 0, size - bytes_read);
    }
}

void WOLVESFileSystem::read_file(uint32_t inode_id, uint32_t file_offset, void* data, uint32_t size) const {
    std::shared_lock<std::shared_mutex> lock(vfs_mutex_);
    read_file_unlocked(inode_id, file_offset, data, size);
}

void WOLVESFileSystem::delete_file(uint32_t inode_id) {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);

    auto it = inodes_.find(inode_id);
    if (it == inodes_.end() || it->second.is_directory) {
        return;
    }

    Package pkg = {};
    pkg.magic = PACKAGE_MAGIC;
    pkg.seq_num = current_seq_num_++;
    pkg.op = static_cast<uint8_t>(OpType::DELETE_FILE);
    pkg.flags = PKG_FLAG_NONE;
    pkg.inode_id = inode_id;
    pkg.parent_inode_id = it->second.parent_id;
    std::strncpy(pkg.filename, it->second.name.c_str(), sizeof(pkg.filename) - 1);
    pkg.checksum = calculate_checksum(pkg);

    append_package(pkg);

    auto p_it = inodes_.find(it->second.parent_id);
    if (p_it != inodes_.end()) {
        auto& c = p_it->second.children_inodes;
        c.erase(std::remove(c.begin(), c.end(), inode_id), c.end());
    }

    inodes_.erase(inode_id);
}

void WOLVESFileSystem::delete_directory(uint32_t inode_id) {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);

    if (inode_id == 1) {
        throw std::runtime_error("Cannot delete root directory!");
    }

    auto it = inodes_.find(inode_id);
    if (it == inodes_.end() || !it->second.is_directory) {
        throw std::runtime_error("Directory does not exist!");
    }

    if (!it->second.children_inodes.empty()) {
        throw std::runtime_error("Directory is not empty!");
    }

    Package pkg = {};
    pkg.magic = PACKAGE_MAGIC;
    pkg.seq_num = current_seq_num_++;
    pkg.op = static_cast<uint8_t>(OpType::DELETE_FILE);
    pkg.flags = PKG_FLAG_NONE;
    pkg.inode_id = inode_id;
    pkg.parent_inode_id = it->second.parent_id;
    std::strncpy(pkg.filename, it->second.name.c_str(), sizeof(pkg.filename) - 1);
    pkg.checksum = calculate_checksum(pkg);

    append_package(pkg);

    auto p_it = inodes_.find(it->second.parent_id);
    if (p_it != inodes_.end()) {
        auto& c = p_it->second.children_inodes;
        c.erase(std::remove(c.begin(), c.end(), inode_id), c.end());
    }

    inodes_.erase(inode_id);
}

void WOLVESFileSystem::rename(const std::string& old_path, const std::string& new_path) {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);

    uint32_t src_id = resolve_path_unlocked(old_path);
    if (src_id == 0 || src_id == 1) {
        throw std::runtime_error("Source file/dir does not exist or is root: " + old_path);
    }

    std::string new_parent_path, new_name;
    split_path(new_path, new_parent_path, new_name);

    uint32_t new_parent_id = resolve_path_unlocked(new_parent_path);
    if (new_parent_id == 0 || !inodes_[new_parent_id].is_directory) {
        throw std::runtime_error("Destination parent directory does not exist: " + new_parent_path);
    }

    uint32_t dest_id = resolve_path_unlocked(new_path);
    uint64_t unlink_pkg_offset = 0;
    uint8_t flags = PKG_FLAG_NONE;

    if (dest_id != 0) {
        if (inodes_[dest_id].is_directory && !inodes_[dest_id].children_inodes.empty()) {
            throw std::runtime_error("Cannot overwrite non-empty directory!");
        }

        Package unlink_pkg = {};
        unlink_pkg.magic = PACKAGE_MAGIC;
        unlink_pkg.seq_num = current_seq_num_++;
        unlink_pkg.op = static_cast<uint8_t>(OpType::DELETE_FILE);
        unlink_pkg.flags = PKG_FLAG_NONE;
        unlink_pkg.inode_id = dest_id;
        unlink_pkg.parent_inode_id = new_parent_id;
        unlink_pkg.checksum = calculate_checksum(unlink_pkg);

        unlink_pkg_offset = append_package(unlink_pkg);
        flags |= PKG_FLAG_COMPOUND;

        auto& d_children = inodes_[new_parent_id].children_inodes;
        d_children.erase(std::remove(d_children.begin(), d_children.end(), dest_id), d_children.end());
        inodes_.erase(dest_id);
    }

    Package rename_pkg = {};
    rename_pkg.magic = PACKAGE_MAGIC;
    rename_pkg.seq_num = current_seq_num_++;
    rename_pkg.op = static_cast<uint8_t>(OpType::RENAME);
    rename_pkg.flags = flags;
    rename_pkg.inode_id = src_id;
    rename_pkg.parent_inode_id = new_parent_id;
    rename_pkg.fwd_ptr = unlink_pkg_offset;
    std::strncpy(rename_pkg.filename, new_name.c_str(), sizeof(rename_pkg.filename) - 1);
    rename_pkg.checksum = calculate_checksum(rename_pkg);

    append_package(rename_pkg);

    uint32_t old_parent_id = inodes_[src_id].parent_id;
    auto& old_children = inodes_[old_parent_id].children_inodes;
    old_children.erase(std::remove(old_children.begin(), old_children.end(), src_id), old_children.end());

    inodes_[src_id].parent_id = new_parent_id;
    inodes_[src_id].name = new_name;
    inodes_[new_parent_id].children_inodes.push_back(src_id);
}

void WOLVESFileSystem::create_snapshot(const std::string& snapshot_name) {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);

    Package snap_pkg = {};
    snap_pkg.magic = PACKAGE_MAGIC;
    snap_pkg.seq_num = current_seq_num_++;
    snap_pkg.op = static_cast<uint8_t>(OpType::SNAPSHOT);
    snap_pkg.flags = PKG_FLAG_NONE;
    snap_pkg.offset = snap_pkg.seq_num;
    std::strncpy(snap_pkg.filename, snapshot_name.c_str(), sizeof(snap_pkg.filename) - 1);
    snap_pkg.checksum = calculate_checksum(snap_pkg);

    append_package(snap_pkg);

    snapshots_.push_back({snapshot_name, snap_pkg.offset, static_cast<uint32_t>(inodes_.size())});
    std::cout << "[SNAPSHOT] Created instant snapshot: \"" << snapshot_name 
              << "\" at Seq=" << snap_pkg.offset << " (Nodes=" << inodes_.size() << ")\n";
}

std::vector<SnapshotInfo> WOLVESFileSystem::list_snapshots() const {
    std::shared_lock<std::shared_mutex> lock(vfs_mutex_);
    return snapshots_;
}

void WOLVESFileSystem::restore_snapshot(const std::string& snapshot_name) {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);

    for (const auto& s : snapshots_) {
        if (s.name == snapshot_name) {
            std::cout << "[SNAPSHOT] Rolling back to snapshot \"" << snapshot_name 
                      << "\" (Threshold Seq=" << s.seq_num << ")...\n";
            package_translation_layer(s.seq_num);
            std::cout << "[SNAPSHOT] Rollback complete. Active VFS nodes: " << inodes_.size() << "\n";
            return;
        }
    }
    throw std::runtime_error("Snapshot not found: " + snapshot_name);
}

void WOLVESFileSystem::rollback_to_seq(uint32_t target_seq) {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);
    package_translation_layer(target_seq);
}

GCStats WOLVESFileSystem::get_gc_stats_unlocked() const {
    GCStats stats = {};
    stats.total_pmem_bytes = pmem_->get_size();
    stats.allocated_bytes = pmem_->get_allocated_offset();
    stats.active_4k_groups = pmem_->get_active_group_count();

    size_t total_groups = pmem_->get_total_groups();
    for (size_t g = 1; g < total_groups; ++g) {
        if (!pmem_->is_group_active(g)) continue;

        uint64_t group_start = g * PKG_GROUP_SIZE;
        uint64_t group_end = std::min<uint64_t>(group_start + PKG_GROUP_SIZE, pmem_->get_size());
        uint64_t offset = group_start;

        while (offset + sizeof(Package) <= group_end) {
            Package pkg;
            pmem_->read(offset, &pkg, sizeof(Package));
            if (pkg.magic == PACKAGE_MAGIC && pkg.checksum == calculate_checksum(pkg)) {
                stats.total_packages++;
                offset += sizeof(Package);
            } else {
                offset += 8;
            }
        }
    }

    size_t live_pkgs = 0;
    size_t live_data = 0;
    for (const auto& pair : inodes_) {
        if (pair.first == 1) continue;
        live_pkgs++;
        live_pkgs += pair.second.extents.size();
        for (const auto& ext : pair.second.extents) {
            live_data += ext.size;
        }
    }
    live_pkgs += snapshots_.size();

    stats.live_packages = live_pkgs;
    stats.dead_packages = (stats.total_packages >= live_pkgs) ? (stats.total_packages - live_pkgs) : 0;

    size_t live_total_bytes = (live_pkgs * sizeof(Package)) + live_data;
    stats.live_bytes = live_total_bytes;
    stats.dead_bytes = (stats.allocated_bytes >= live_total_bytes) ? (stats.allocated_bytes - live_total_bytes) : 0;

    return stats;
}

GCStats WOLVESFileSystem::get_gc_stats() const {
    std::shared_lock<std::shared_mutex> lock(vfs_mutex_);
    return get_gc_stats_unlocked();
}

void WOLVESFileSystem::compact() {
    std::unique_lock<std::shared_mutex> lock(vfs_mutex_);

    std::cout << "[GC] Initiating Storage Compaction & Garbage Collection...\n";
    GCStats before = get_gc_stats_unlocked();
    std::cout << "[GC] Before: " << before.allocated_bytes << " B allocated across " 
              << before.active_4k_groups << " active 4KB groups. Dead space: " 
              << before.dead_bytes << " B.\n";

    std::vector<uint8_t> new_mem(pmem_->get_size(), 0);
    uint64_t new_cursor = SUPERBLOCK_SIZE;
    uint32_t new_seq = 1;

    auto append_compact_pkg = [&](Package& pkg) -> uint64_t {
        if (new_cursor % 8 != 0) new_cursor += 8 - (new_cursor % 8);
        if ((new_cursor / PKG_GROUP_SIZE) != ((new_cursor + sizeof(Package) - 1) / PKG_GROUP_SIZE)) {
            new_cursor = ((new_cursor / PKG_GROUP_SIZE) + 1) * PKG_GROUP_SIZE;
        }
        uint64_t pos = new_cursor;
        pkg.seq_num = new_seq++;
        pkg.checksum = calculate_checksum(pkg);
        std::memcpy(new_mem.data() + pos, &pkg, sizeof(Package));
        new_cursor += sizeof(Package);
        return pos;
    };

    auto append_compact_data = [&](const void* data, size_t size) -> uint64_t {
        if (new_cursor % 8 != 0) new_cursor += 8 - (new_cursor % 8);
        uint64_t pos = new_cursor;
        std::memcpy(new_mem.data() + pos, data, size);
        new_cursor += size;
        return pos;
    };

    // 1. Re-persist surviving directories
    for (const auto& pair : inodes_) {
        if (pair.first == 1 || !pair.second.is_directory) continue;
        Package pkg = {};
        pkg.magic = PACKAGE_MAGIC;
        pkg.op = static_cast<uint8_t>(OpType::CREATE_DIR);
        pkg.flags = PKG_FLAG_NONE;
        pkg.inode_id = pair.second.id;
        pkg.parent_inode_id = pair.second.parent_id;
        std::strncpy(pkg.filename, pair.second.name.c_str(), sizeof(pkg.filename) - 1);
        append_compact_pkg(pkg);
    }

    // 2. Re-persist surviving files with consolidated data
    for (const auto& pair : inodes_) {
        if (pair.first == 1 || pair.second.is_directory) continue;

        Package c_pkg = {};
        c_pkg.magic = PACKAGE_MAGIC;
        c_pkg.op = static_cast<uint8_t>(OpType::CREATE_FILE);
        c_pkg.flags = PKG_FLAG_NONE;
        c_pkg.inode_id = pair.second.id;
        c_pkg.parent_inode_id = pair.second.parent_id;
        std::strncpy(c_pkg.filename, pair.second.name.c_str(), sizeof(c_pkg.filename) - 1);
        append_compact_pkg(c_pkg);

        if (pair.second.size > 0) {
            std::vector<uint8_t> file_buf(pair.second.size, 0);
            read_file_unlocked(pair.second.id, 0, file_buf.data(), pair.second.size);

            uint64_t data_pos = append_compact_data(file_buf.data(), file_buf.size());

            Package w_pkg = {};
            w_pkg.magic = PACKAGE_MAGIC;
            w_pkg.op = static_cast<uint8_t>(OpType::WRITE_DATA);
            w_pkg.flags = PKG_FLAG_NONE;
            w_pkg.inode_id = pair.second.id;
            w_pkg.parent_inode_id = pair.second.parent_id;
            w_pkg.offset = 0;
            w_pkg.size = pair.second.size;
            w_pkg.data_pmem_ptr = data_pos;
            std::strncpy(w_pkg.filename, pair.second.name.c_str(), sizeof(w_pkg.filename) - 1);
            append_compact_pkg(w_pkg);
        }
    }

    // 3. Re-persist active snapshots
    for (const auto& s : snapshots_) {
        Package s_pkg = {};
        s_pkg.magic = PACKAGE_MAGIC;
        s_pkg.op = static_cast<uint8_t>(OpType::SNAPSHOT);
        s_pkg.flags = PKG_FLAG_NONE;
        s_pkg.offset = s.seq_num;
        std::strncpy(s_pkg.filename, s.name.c_str(), sizeof(s_pkg.filename) - 1);
        append_compact_pkg(s_pkg);
    }

    pmem_->replace_memory(new_mem, new_cursor);
    package_translation_layer(UINT32_MAX);

    GCStats after = get_gc_stats_unlocked();
    std::cout << "[GC] Compaction complete! Reclaimed " << before.dead_bytes 
              << " bytes. Allocated size is now " << after.allocated_bytes 
              << " B across " << after.active_4k_groups << " active 4KB groups.\n";
}

const Inode* WOLVESFileSystem::get_inode(uint32_t inode_id) const {
    std::shared_lock<std::shared_mutex> lock(vfs_mutex_);
    auto it = inodes_.find(inode_id);
    return (it != inodes_.end()) ? &it->second : nullptr;
}

size_t WOLVESFileSystem::get_file_size(uint32_t inode_id) const {
    std::shared_lock<std::shared_mutex> lock(vfs_mutex_);
    auto it = inodes_.find(inode_id);
    return (it != inodes_.end()) ? it->second.size : 0;
}

bool WOLVESFileSystem::is_directory(uint32_t inode_id) const {
    std::shared_lock<std::shared_mutex> lock(vfs_mutex_);
    auto it = inodes_.find(inode_id);
    return (it != inodes_.end()) && it->second.is_directory;
}

std::vector<std::string> WOLVESFileSystem::list_directory(uint32_t dir_inode_id) const {
    std::shared_lock<std::shared_mutex> lock(vfs_mutex_);
    std::vector<std::string> results;
    auto it = inodes_.find(dir_inode_id);
    if (it != inodes_.end() && it->second.is_directory) {
        for (uint32_t child_id : it->second.children_inodes) {
            auto c_it = inodes_.find(child_id);
            if (c_it != inodes_.end()) {
                results.push_back(c_it->second.name);
            }
        }
    }
    return results;
}

void WOLVESFileSystem::print_tree(uint32_t dir_id, int indent) const {
    std::shared_lock<std::shared_mutex> lock(vfs_mutex_);
    auto it = inodes_.find(dir_id);
    if (it == inodes_.end()) return;

    std::string prefix(indent * 2, ' ');
    if (dir_id == 1) {
        std::cout << prefix << "/ (root directory)\n";
    }

    for (uint32_t child_id : it->second.children_inodes) {
        auto c_it = inodes_.find(child_id);
        if (c_it == inodes_.end()) continue;

        if (c_it->second.is_directory) {
            std::cout << prefix << "  📁 [" << c_it->second.name << "] (Inode=" << child_id << ")\n";
            print_tree(child_id, indent + 1);
        } else {
            std::cout << prefix << "  📄 " << c_it->second.name << " (Inode=" << child_id 
                      << ", Size=" << c_it->second.size << " B, Extents=" 
                      << c_it->second.extents.size() << ")\n";
        }
    }
}

} // namespace wofs
