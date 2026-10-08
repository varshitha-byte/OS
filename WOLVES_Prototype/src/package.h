#pragma once
#include <cstdint>
#include <cstddef>

namespace wofs {

// The types of operations recorded in packages
enum class OpType : uint8_t {
    INVALID = 0,
    CREATE_FILE = 1,
    WRITE_DATA = 2,
    DELETE_FILE = 3
};

// Represents a write-once package as described in WOLVES paper.
// "WOFS generates specific metadata for each file operation as a checksum-protected 
// package and writes it once with a single ordering point."
struct Package {
    uint32_t magic;         // Magic number (e.g., 0x574F4653 'WOFS')
    uint32_t checksum;      // Checksum over the rest of the package
    uint32_t seq_num;       // Sequence number to enforce global ordering during recovery
    OpType op;              // Type of operation
    uint32_t inode_id;      // Target file identifier
    uint32_t offset;        // For WRITE_DATA, the offset in the file
    uint32_t size;          // For WRITE_DATA, size of data
    uint64_t data_pmem_ptr; // Simulated pointer/offset to the actual data blocks in PM
    char filename[32];      // For CREATE_FILE
};

constexpr uint32_t PACKAGE_MAGIC = 0x574F4653;

// A simple utility to calculate a basic checksum
inline uint32_t calculate_checksum(const Package& pkg) {
    uint32_t sum = 0;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&pkg);
    // Skip the magic and checksum fields itself (first 8 bytes)
    for (size_t i = 8; i < sizeof(Package); ++i) {
        sum += ptr[i];
    }
    return sum;
}

} // namespace wofs
