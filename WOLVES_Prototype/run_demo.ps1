Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Running WOLVES Prototype: Crash & Recovery Simulator " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# Run the compiled binary inside WSL Ubuntu
wsl -d Ubuntu -e bash -c "cd /mnt/c/Users/varsh/OneDrive/Desktop/OS/WOLVES_Prototype/build_linux && ./wolves_sim"

Write-Host ""
Write-Host "==========================================================" -ForegroundColor Green
Write-Host " Inspecting Raw Binary Storage (simulated_pmem.bin) " -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Green

# Inspect the persistent memory binary image
python "C:\Users\varsh\OneDrive\Desktop\OS\WOLVES_Prototype\inspect_pmem.py"

Write-Host ""
Write-Host "==========================================================" -ForegroundColor Yellow
Write-Host " Running Chaos Engineering & Crash Fuzzer (Track D) " -ForegroundColor Yellow
Write-Host "==========================================================" -ForegroundColor Yellow

python "C:\Users\varsh\OneDrive\Desktop\OS\WOLVES_Prototype\fuzz_crash.py"
