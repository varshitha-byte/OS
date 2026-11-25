#pragma once
#include <cstdint>
#include <cstddef>

namespace wofs {

// The types of operations recorded in packages
enum class OpType : uint8_t {
    INVALID = 0,
    CREATE_FILE = 1,
    WRITE_DATA = 2,
    DELETE_FILE = 3,
    CREATE_DIR = 4,
    RENAME = 5,
    SNAPSHOT = 6
};

// Flags for packages
constexpr uint8_t PKG_FLAG_NONE     = 0x00;
constexpr uint8_t PKG_FLAG_COMPOUND = 0x01; // Part of a compound operation (uses fwd_ptr)

// Represents a write-once package as described in WOLVES paper.
// 80-byte 8-byte aligned structure with zero padding gaps between multi-byte fields.
#pragma pack(push, 1)
struct Package {
    uint32_t magic;           // 4 bytes: Magic number (0x574F4653 'WOFS')
    uint32_t checksum;        // 4 bytes: Checksum over the rest of the package
    uint32_t seq_num;         // 4 bytes: Sequence number for ordering
    uint8_t  op;              // 1 byte:  OpType
    uint8_t  flags;           // 1 byte:  Compound flags
    uint16_t reserved1;       // 2 bytes: Alignment padding
    uint32_t inode_id;        // 4 bytes: Target file / directory identifier
    uint32_t parent_inode_id; // 4 bytes: Parent directory inode identifier
    uint32_t offset;          // 4 bytes: For WRITE_DATA: file offset; For SNAPSHOT: target_seq_num
    uint32_t size;            // 4 bytes: For WRITE_DATA: size of data payload
    uint64_t data_pmem_ptr;   // 8 bytes: Simulated pointer to raw data blocks in PM
    uint64_t fwd_ptr;         // 8 bytes: Forward pointer for compound packages
    char     filename[32];    // 32 bytes: File / Directory / Snapshot name (null-terminated)
};
#pragma pack(pop)

static_assert(sizeof(Package) == 80, "Package struct must be exactly 80 bytes for PM alignment");

constexpr uint32_t PACKAGE_MAGIC = 0x574F4653; // 'WOFS' in ASCII

// Checksum calculation over package content skipping magic and checksum fields
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
