#include "pmem.h"
#include "wofs.h"
#include <iostream>
#include <memory>
#include <cassert>
#include <cstring>
#include <cstdio>

using namespace wofs;

void run_simulation(bool simulate_crash) {
    const std::string pm_file = "simulated_pmem.bin";
    
    // Simulate 100MB of PMEM
    auto pmem = std::make_shared<PMemSimulator>(pm_file, 100 * 1024 * 1024);
    
    WOLVESFileSystem fs(pmem);
    fs.mount();

    if (!simulate_crash) {
        std::cout << "\n=== Phase 1: Creating and writing to files ===\n";
        
        uint32_t fd1 = fs.create_file("document.txt");
        uint32_t fd2 = fs.create_file("image.png");

        std::string text = "Hello WOLVES! This is a test of the Metadata Write-Once File System.";
        fs.write_file(fd1, 0, text.c_str(), text.size() + 1);

        std::string img_data = "\x89PNG\r\n\x1a\n..."; // Dummy
        fs.write_file(fd2, 0, img_data.c_str(), img_data.size());

        fs.list_files();

        char buffer[128] = {0};
        fs.read_file(fd1, 0, buffer, text.size() + 1);
        std::cout << "Read from document.txt: " << buffer << "\n";

        std::cout << "\nSimulating system crash... (PMEM simulator goes out of scope and saves to disk)\n";
    } else {
        std::cout << "\n=== Phase 2: Post-Crash Recovery ===\n";
        fs.list_files();

        // Let's try reading the file that was created before the crash
        // Since we know document.txt was likely fd 1, but we can just do a test:
        try {
            char buffer[128] = {0};
            std::string text = "Hello WOLVES! This is a test of the Metadata Write-Once File System.";
            fs.read_file(1, 0, buffer, text.size() + 1);
            std::cout << "Successfully read recovered file (Inode 1): " << buffer << "\n";
            
            if (strcmp(buffer, text.c_str()) == 0) {
                std::cout << "[SUCCESS] The data survived the simulated crash!\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to read recovered file: " << e.what() << "\n";
        }
    }
}

int main() {
    std::cout << "--- Starting WOLVES File System Prototype ---\n";
    
    // Clean up any old simulated PM
    std::remove("simulated_pmem.bin");

    // Run normally, then "crash"
    run_simulation(false);
    
    // Now restart and let the translation layer recover from PMEM
    run_simulation(true);

    return 0;
}
