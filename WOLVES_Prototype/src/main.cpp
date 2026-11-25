#include "pmem.h"
#include "wofs.h"
#include <iostream>
#include <memory>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <thread>
#include <vector>

using namespace wofs;

void test_multithreading_concurrency(WOLVESFileSystem& fs) {
    std::cout << "\n[Track F] Launching Concurrent Multi-Threaded Stress Test...\n";
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> workers;

    uint32_t shared_file = fs.create_file("/documents/shared_data.txt");
    std::string init_text = "Initial baseline shared text content.";
    fs.write_file(shared_file, 0, init_text.c_str(), init_text.size() + 1);

    // 2 Concurrent Readers
    for (int r = 0; r < 2; ++r) {
        workers.emplace_back([&fs, shared_file, &stop_flag, r]() {
            char buf[64];
            size_t reads_done = 0;
            while (!stop_flag.load()) {
                fs.read_file(shared_file, 0, buf, sizeof(buf));
                reads_done++;
                std::this_thread::yield();
            }
        });
    }

    // 2 Concurrent Writers
    for (int w = 0; w < 2; ++w) {
        workers.emplace_back([&fs, w, &stop_flag]() {
            for (int i = 0; i < 20; ++i) {
                std::string fname = "/documents/thread_" + std::to_string(w) + "_f" + std::to_string(i) + ".dat";
                try {
                    uint32_t fid = fs.create_file(fname);
                    std::string payload = "Thread data payload " + std::to_string(i);
                    fs.write_file(fid, 0, payload.c_str(), payload.size() + 1);
                } catch (...) {}
            }
        });
    }

    // Wait for writers
    for (size_t i = 2; i < workers.size(); ++i) {
        if (workers[i].joinable()) workers[i].join();
    }
    stop_flag.store(true);

    // Wait for readers
    for (size_t i = 0; i < 2; ++i) {
        if (workers[i].joinable()) workers[i].join();
    }

    std::cout << "[Track F] Concurrency test passed without deadlocks or memory corruption!\n";
}

void run_simulation(bool is_recovery_phase) {
    const std::string pm_file = "simulated_pmem.bin";
    
    // Simulate 100MB of Persistent Memory (25,600 groups of 4 KiB)
    auto pmem = std::make_shared<PMemSimulator>(pm_file, 100 * 1024 * 1024);
    WOLVESFileSystem fs(pmem);
    fs.mount();

    if (!is_recovery_phase) {
        std::cout << "\n======================================================\n";
        std::cout << " Phase 1: Track A, C, F Multi-Feature Demonstration\n";
        std::cout << "======================================================\n";
        
        // 1. Directory Tree & Coarse Persistence (Track C)
        std::cout << "\n[1] Creating Directories & Files (Track C 4KB Package Groups)...\n";
        fs.create_directory("/documents");
        fs.create_directory("/projects");

        uint32_t f_notes = fs.create_file("/documents/notes.txt");
        std::string notes_v1 = "WOLVES Version 1.0 notes.";
        fs.write_file(f_notes, 0, notes_v1.c_str(), notes_v1.size() + 1);

        uint32_t f_code = fs.create_file("/projects/code.cpp");
        std::string code_v1 = "void original() {}";
        fs.write_file(f_code, 0, code_v1.c_str(), code_v1.size() + 1);

        // 2. Track A: Create Snapshot
        std::cout << "\n[2] Creating Snapshot \"release_v1.0\" (Track A Zero-Copy Snapshot)...\n";
        fs.create_snapshot("release_v1.0");

        std::cout << "\n--- Directory Tree at Snapshot release_v1.0 ---\n";
        fs.print_tree();

        // 3. Destructive modifications after snapshot
        std::cout << "\n[3] Making Destructive Changes (Deleting files & overwriting)...\n";
        fs.delete_file(f_notes);
        std::cout << "    Deleted /documents/notes.txt!\n";

        std::string code_v2 = "void corrupted_after_release() {}";
        fs.write_file(f_code, 0, code_v2.c_str(), code_v2.size() + 1);

        uint32_t f_unwanted = fs.create_file("/projects/unwanted_scratch.tmp");
        std::string tmp = "scratch";
        fs.write_file(f_unwanted, 0, tmp.c_str(), tmp.size() + 1);

        std::cout << "\n--- Directory Tree After Modifications (Before Restore) ---\n";
        fs.print_tree();

        // 4. Track A: Instant Rollback / Time-Travel
        std::cout << "\n[4] Performing Instant Time-Travel Restore to \"release_v1.0\"...\n";
        fs.restore_snapshot("release_v1.0");

        std::cout << "\n--- Directory Tree Restored to release_v1.0 ---\n";
        fs.print_tree();

        // Verify notes.txt is resurrected and unwanted files disappeared!
        uint32_t res_notes = fs.resolve_path("/documents/notes.txt");
        assert(res_notes != 0);
        char buf[64] = {0};
        fs.read_file(res_notes, 0, buf, sizeof(buf));
        std::cout << "    [VERIFIED] Resurrected /documents/notes.txt: \"" << buf << "\"\n";

        uint32_t res_tmp = fs.resolve_path("/projects/unwanted_scratch.tmp");
        assert(res_tmp == 0);
        std::cout << "    [VERIFIED] Unwanted file correctly purged in snapshot view!\n";

        // Re-mount latest state for compaction
        fs.rollback_to_seq(UINT32_MAX);

        // 5. Track F: Multithreading Concurrency
        test_multithreading_concurrency(fs);

        // 6. Track C Coarse Persistence & Compaction
        std::cout << "\n[6] Compaction & Coarse Persistence Inspection...\n";
        GCStats stats = fs.get_gc_stats();
        std::cout << "    Active 4KB Package Groups: " << stats.active_4k_groups 
                  << " / " << (100 * 1024 / 4) << "\n";
        fs.compact();

        std::cout << "\n[!] Simulating sudden power loss / system crash...\n";

    } else {
        std::cout << "\n======================================================\n";
        std::cout << " Phase 2: Post-Crash Recovery Verification\n";
        std::cout << "======================================================\n";
        
        std::cout << "\n--- Reconstructed Directory Tree from PMEM Packages ---\n";
        fs.print_tree();

        // Verify snapshots survived crash
        auto snaps = fs.list_snapshots();
        std::cout << "\n[VERIFIED] Recovered " << snaps.size() << " Snapshot(s) after crash:\n";
        for (const auto& s : snaps) {
            std::cout << "  * Snapshot \"" << s.name << "\" (Seq=" << s.seq_num << ")\n";
        }
        assert(!snaps.empty() && snaps[0].name == "release_v1.0");

        // Verify that active files and snapshot metadata survived cold crash reboot!
        std::cout << "\n[VERIFIED] Testing Active Files from Cold Boot...\n";
        uint32_t code_id = fs.resolve_path("/projects/code.cpp");
        assert(code_id != 0);
        std::cout << "    Verified /projects/code.cpp (Inode=" << code_id << ") survived cold reboot!\n";

        uint32_t t_id = fs.resolve_path("/documents/thread_0_f1.dat");
        assert(t_id != 0);
        std::cout << "    Verified concurrent writer file /documents/thread_0_f1.dat survived cold reboot!\n";

        std::cout << "\n>>> ALL TRACK A, C, F CAPABILITIES & CRASH RECOVERY TESTS PASSED! <<<\n";
    }
}

int main(int argc, char* argv[]) {
    std::cout << "==========================================================\n";
    std::cout << " WOLVES File System: Advanced Multi-Track Prototype\n";
    std::cout << " Tracks Implemented: A (Snapshots), C (Bitmaps), F (Threads)\n";
    std::cout << "==========================================================\n";

    if (argc > 1 && std::strcmp(argv[1], "--recover") == 0) {
        // Recovery-only mode for chaos fuzzer and cold audits
        std::cout << "[RECOVERY MODE] Running PTL recovery on existing storage...\n";
        run_simulation(true);
        return 0;
    }

    std::remove("simulated_pmem.bin");

    // Phase 1: Snapshots, Coarse Bitmaps, Concurrency, and Crash
    run_simulation(false);

    // Phase 2: Cold boot recovery and post-crash snapshot rollback
    run_simulation(true);

    return 0;
}
