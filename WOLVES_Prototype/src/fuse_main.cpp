#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <memory>
#include "wofs.h"

// Global pointer to our file system instance
static std::unique_ptr<wofs::WOLVESFileSystem> wolves_fs;

static int wofs_getattr(const char *path, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    uint32_t inode_id = wolves_fs->find_file(path);
    if (inode_id > 0) {
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = wolves_fs->get_file_size(inode_id);
        return 0;
    }

    return -ENOENT;
}

static int wofs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;

    if (strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    auto files = wolves_fs->get_all_filenames();
    for (const auto& name : files) {
        filler(buf, name.c_str(), NULL, 0);
    }

    return 0;
}

static int wofs_open(const char *path, struct fuse_file_info *fi) {
    uint32_t inode_id = wolves_fs->find_file(path);
    if (inode_id == 0)
        return -ENOENT;
        
    // Store inode_id in the file info for fast access during read/write
    fi->fh = inode_id;
    return 0;
}

static int wofs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    uint32_t inode_id = fi->fh;
    if (inode_id == 0) return -ENOENT;

    size_t file_size = wolves_fs->get_file_size(inode_id);
    if (offset >= file_size) return 0;

    if (offset + size > file_size)
        size = file_size - offset;

    wolves_fs->read_file(inode_id, offset, buf, size);
    return size;
}

static int wofs_write(const char *path, const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    uint32_t inode_id = fi->fh;
    if (inode_id == 0) return -ENOENT;

    wolves_fs->write_file(inode_id, offset, buf, size);
    return size;
}

static int wofs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    std::string name = path;
    if (!name.empty() && name[0] == '/') name = name.substr(1);
    
    uint32_t inode_id = wolves_fs->create_file(name);
    fi->fh = inode_id;
    return 0;
}

static int wofs_unlink(const char *path) {
    uint32_t inode_id = wolves_fs->find_file(path);
    if (inode_id == 0) return -ENOENT;

    wolves_fs->delete_file(inode_id);
    return 0;
}

static int wofs_truncate(const char *path, off_t size) {
    // Basic support to satisfy standard file tools;
    // Real truncation requires updating package metadata and removing trailing extents.
    return 0;
}

static struct fuse_operations wofs_oper = {};

int main(int argc, char *argv[]) {
    wofs_oper.getattr = wofs_getattr;
    wofs_oper.readdir = wofs_readdir;
    wofs_oper.open    = wofs_open;
    wofs_oper.read    = wofs_read;
    wofs_oper.write   = wofs_write;
    wofs_oper.create  = wofs_create;
    wofs_oper.unlink  = wofs_unlink;
    wofs_oper.truncate = wofs_truncate;

    // Initialize our PM Simulator and File System
    auto pmem = std::make_shared<wofs::PMemSimulator>("wolves_fuse_pmem.bin", 200 * 1024 * 1024);
    wolves_fs = std::make_unique<wofs::WOLVESFileSystem>(pmem);
    wolves_fs->mount();

    return fuse_main(argc, argv, &wofs_oper, NULL);
}
