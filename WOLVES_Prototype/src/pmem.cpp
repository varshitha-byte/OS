#include "pmem.h"
#include <fstream>
#include <iostream>
#include <cstring>

namespace wofs {

PMemSimulator::PMemSimulator(const std::string& backing_file, size_t size)
    : backing_file_(backing_file), next_free_offset_(0) {
    memory_.resize(size, 0);
    load_from_disk();
}

PMemSimulator::~PMemSimulator() {
    save_to_disk();
}

uint64_t PMemSimulator::allocate(size_t size) {
    // Align allocations to 8 bytes to make recovery scanning reliable
    if (next_free_offset_ % 8 != 0) {
        next_free_offset_ += 8 - (next_free_offset_ % 8);
    }
    
    if (next_free_offset_ + size > memory_.size()) {
        throw std::runtime_error("Out of simulated PMEM space!");
    }
    uint64_t allocated = next_free_offset_;
    next_free_offset_ += size;
    return allocated;
}

void PMemSimulator::write(uint64_t offset, const void* data, size_t size) {
    if (offset + size > memory_.size()) {
        throw std::runtime_error("Write out of bounds!");
    }
    std::memcpy(memory_.data() + offset, data, size);
}

void PMemSimulator::read(uint64_t offset, void* data, size_t size) const {
    if (offset + size > memory_.size()) {
        throw std::runtime_error("Read out of bounds!");
    }
    std::memcpy(data, memory_.data() + offset, size);
}

void PMemSimulator::persist(uint64_t offset, size_t size) {
    // In a real PM system, this would be clwb/clflushopt + sfence.
    // For simulation, we just save the whole buffer to disk.
    // (A real implementation would flush specific cache lines.)
    save_to_disk();
}

void PMemSimulator::load_from_disk() {
    std::ifstream in(backing_file_, std::ios::binary);
    if (in) {
        in.read(reinterpret_cast<char*>(memory_.data()), memory_.size());
        
        // Simple recovery of allocation pointer (find last non-zero byte roughly)
        // For WOLVES, packages are appended. Let's just find the last non-zero block.
        // Actually, the FS layer should manage its own allocator, but this is a simple simulator.
        // We'll scan from end to find the first non-zero 8 bytes.
        next_free_offset_ = 0;
        for (size_t i = 0; i < memory_.size(); i += 8) {
            bool empty = true;
            for (size_t j = 0; j < 8 && (i+j) < memory_.size(); ++j) {
                if (memory_[i+j] != 0) { empty = false; break; }
            }
            if (!empty) {
                next_free_offset_ = i + 8;
            }
        }
    }
}

void PMemSimulator::save_to_disk() {
    std::ofstream out(backing_file_, std::ios::binary);
    if (out) {
        out.write(reinterpret_cast<const char*>(memory_.data()), memory_.size());
    }
}

} // namespace wofs
