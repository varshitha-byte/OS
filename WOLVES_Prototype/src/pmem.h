#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wofs {

class PMemSimulator {
public:
    PMemSimulator(const std::string& backing_file, size_t size);
    ~PMemSimulator();

    // Allocate continuous space in PM
    uint64_t allocate(size_t size);

    // Write to a specific offset in PM
    void write(uint64_t offset, const void* data, size_t size);

    // Read from a specific offset in PM
    void read(uint64_t offset, void* data, size_t size) const;

    // Simulates the PCOMMIT + sfence operation
    void persist(uint64_t offset, size_t size);

    // Simulated PM base address for direct access (if needed)
    uint8_t* get_base() { return memory_.data(); }
    size_t get_size() const { return memory_.size(); }

private:
    std::string backing_file_;
    std::vector<uint8_t> memory_;
    uint64_t next_free_offset_;

    void load_from_disk();
    void save_to_disk();
};

} // namespace wofs
