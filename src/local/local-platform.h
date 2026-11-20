#pragma once
// Host-OS helpers for the local performance and diagnostic code (thread names, thread
// ids, CPU pinning), so the local headers stay free of <windows.h> and Linux-only APIs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace LocalPlatform {

// Names the calling thread (Linux: 15 characters are kept).
void SetThreadName(const char* name);

// The OS id of the calling thread (Linux: gettid, Windows: GetCurrentThreadId).
uint32_t ThreadId();

// Pins the calling thread to a comma-separated CPU list ("1,2,3"); an empty or invalid
// list leaves the affinity alone.
void PinThreadToCpuList(const char* list);

// Pins a thread (a CurrentThreadHandle) to a CPU list; false if the list or the call fails.
bool PinThreadHandleToCpuList(uint64_t handle, const char* list);

// Windows: the process's default CPU set as a CPU list; "" restores all CPUs. Linux: false.
bool SetProcessDefaultCpuList(const char* list);

// Windows: the speculation workers' default CPU list: the performance cores within the process's CPUs (Windows' CPU
// set efficiency classes; every core where all are alike) without the core of CPU 0, as run-windows.ps1's "auto"
// picks the render threads' CPUs; of those, the upper half of the cores (rounded up) with all their logical CPUs:
// "8,9,10,11,12,13,14,15" on an 8 P-core + 16 E-core part (P-cores 1-7, CPUs 2-15, in the render list). "" when
// there is none. Linux: "".
std::string SpeculationCpuList();
// The logical CPUs of the performance cores (the highest efficiency class; all of them where every core is alike) the
// process may run on; 0 where that is not known (Linux). KYTY_SIMULATE_PCORES=<n> stands in for it (a test).
uint32_t PerformanceLogicalCpus();

// Opaque handle of the calling thread for ThreadCpuSeconds (valid while the thread runs).
uint64_t CurrentThreadHandle();
double   ThreadCpuSeconds(uint64_t handle);
// The calling thread's run time in TSC ticks (Windows: QueryThreadCycleTime; exact, unlike the
// tick-sampled ThreadCpuSeconds). Linux: 0.
uint64_t CurrentThreadCycles();
// The calling thread's scheduling priority (Windows: THREAD_PRIORITY_*, -2 lowest .. 2 highest; Linux: 0).
int CurrentThreadPriority();
// The idle time of the logical processors this process may run on, summed, in 100 ns units, and which those are
// (bit n: processor n; Windows: the process's group, NtQuerySystemInformation; Linux: the main thread's affinity,
// /proc/stat's idle + iowait). False when unknown (macOS).
bool ProcessCpusIdleTime(uint64_t* idle_100ns, uint64_t* cpus);

// Total CPU seconds of the threads with this name (0 where the OS cannot enumerate them).
double NamedThreadsCpuSeconds(const char* name);
// The process's threads: (thread id, name; empty when it has none). Windows only (Linux: none).
std::vector<std::pair<uint32_t, std::string>> ProcessThreads();

// A read-only host file kept open (one handle a path) and its size, for positional reads from any thread
// (ReadOpenFileAt) until the call's DoneKeptReadFile; once unread for a while it is closed (Windows: 10 s,
// KYTY_FILE_IDLE_CLOSE=<seconds>, 0 never) and a later call opens it again. 0: it cannot be opened, or (Linux) none are
// kept: open it for each read.
uint64_t OpenKeptReadFile(const std::string& path, uint64_t* size);
// Up to `size` bytes at `offset` of a kept file; the bytes read (0 at the end or on an error).
uint32_t ReadOpenFileAt(uint64_t file, uint64_t offset, void* buffer, uint32_t size);
// The reads of an OpenKeptReadFile call ended (each call that returned a file has one): its handle is not used any more.
void DoneKeptReadFile(const std::string& path);
// A kept file read whole once, in the background (the first call for the path; Windows only): its later reads come from
// the system file cache.
void PrefetchKeptFile(const std::string& path);
// The game's file opens followed (Windows; once, KYTY_FILE_FOLLOW=0: not): `recording` lists the files OpenKeptReadFile
// opened in earlier sessions in the order first opened (one a line, relative to `root` where under it; this session's
// new ones appended). Each one the game opens, or first reads after it was opened here, has the 1024 recorded after it
// opened on threads of their own, ahead of the game's reads.
void FollowKeptFiles(const std::string& recording, const std::string& root);

// Stack bounds of the calling thread; false when unknown.
bool CurrentThreadStack(uint64_t* low, uint64_t* high);

// Each processor running a thread of the process serializes: their stores made visible, their later loads after
// the caller's stores (the slow side of an asymmetric lock, RenderMutex). False where the host has no such call.
bool FlushProcessWriteBuffers();

// The calling thread kept off the CPUs of a list (KYTY_RENDER_CPUS; null or empty: any CPU); with
// below-normal priority too: background work that must not hold up the game's threads.
void AvoidCpuList(const char* avoid_cpus);
void MakeBackgroundThread(const char* avoid_cpus);
// Linux, the render thread (KYTY_RENDER_CORE=<cpu>; 0 or off: not; default on the Steam Deck build: 2): the calling
// thread on that CPU and the core it is on (its SMT siblings) kept for it alone: every other thread of the process, the
// game's included, is moved off that core now and every 250 ms (a thread of its own; new threads inherit their
// creator's CPUs). On a 4-core CPU the game's ten job workers shared the render thread's core: its work per frame
// 31.5 ms with them, 28.4 ms alone. Returns the CPU (-1: not done). Windows: -1.
int KeepRenderCoreAlone();
// The calling thread above the normal priority of the game's threads (Windows: THREAD_PRIORITY_HIGHEST; Linux:
// nice -10 where the nice limit allows, unless KYTY_LINUX_FAST_LOCKS=0): the render thread, which every frame waits for, is not time-sliced against them on its CPUs.
void MakeCriticalThread();
// While it lives, the calling thread at the normal priority if it was above it (MakeCriticalThread), then back: the render
// thread doing the upload worker's jobs (AsyncUpload's Help) as the worker does them, not ahead of the game's threads.
class NormalPriority {
public:
	NormalPriority();
	~NormalPriority();
	NormalPriority(const NormalPriority&)            = delete;
	NormalPriority& operator=(const NormalPriority&) = delete;

private:
	int  m_saved   = 0;
	bool m_changed = false;
};

// The refresh rate in Hz of the display a window is on (its HWND on Windows; 0 when unknown or elsewhere).
int DisplayRefreshHz(void* window);
// KYTY_RETURN_FOCUS=<window handle> (the launcher's --play: the window that had the focus as it started, Steam's Big
// Picture when Steam starts the game): the focus handed to that window and `window` (the game's, its HWND) hidden, as
// the game quits (issue #217). That window gone or hidden: the largest of KYTY_RETURN_FOCUS_PID=<process> (the
// launcher's parent) or of a process it started. Windows only.
void ReturnFocus(void* window);
// Vertical blanks of that display, counted by a thread of their own that waits for each (Windows:
// D3DKMTWaitForVerticalBlankEvent on the window's display, again when the window moves to another one); a window
// passed again replaces the first. false where there are none.
bool StartDisplayVblanks(void* window);
// Waits up to `timeout_us` for a display vblank after the `seen`th one; `seen` then counts it. Several that came while
// the caller was away are taken one call each (four at most). false on a timeout or without StartDisplayVblanks.
bool WaitDisplayVblank(uint64_t& seen, uint32_t timeout_us);

// A temporary file for scratch data, deleted when it is closed or the process ends (Windows keeps it
// in memory while it can: FILE_ATTRIBUTE_TEMPORARY); 0 when none could be made. Writes and reads go
// to any offset, from any thread.
uint64_t OpenScratchFile();
bool     WriteScratchFile(uint64_t file, uint64_t offset, const void* data, size_t size);
bool     ReadScratchFile(uint64_t file, uint64_t offset, void* data, size_t size);
void     CloseScratchFile(uint64_t file);
// An existing file (a UTF-8 path) for ReadScratchFile and CloseScratchFile; 0 when it cannot be opened.
uint64_t OpenFileForReading(const char* path);
// A file kept between sessions (a UTF-8 path), opened or made for ReadScratchFile, WriteScratchFile and
// CloseScratchFile by this process alone: 0 when it cannot be, or another process has it open (a second emulator).
uint64_t OpenDataFile(const char* path);
// A scratch or data file's size in bytes (0 when unknown), the file cut or extended to `size`, and its written bytes
// sent to the disk (false when any of them fails).
uint64_t ScratchFileSize(uint64_t file);
bool     ResizeScratchFile(uint64_t file, uint64_t size);
bool     FlushScratchFile(uint64_t file);
// This executable's path, UTF-8 (empty when unknown).
std::string ExecutablePath();
// The file system of the drive holding `path` (UTF-8): "NTFS", "exFAT", "FAT32"... ("FAT32" for a Linux vfat mount; empty
// when unknown).
std::string FileSystemName(const std::string& path);

// The process's committed private memory and resident working set, and the machine's physical memory, in
// bytes (0 where the host cannot tell; KYTY_SIMULATE_RAM_MB=<n> reports n MiB of physical memory, for tests).
struct MemoryUse {
	uint64_t private_bytes = 0;
	uint64_t working_set   = 0;
};
MemoryUse ProcessMemory();
uint64_t  PhysicalMemory();
// The physical memory the system can still give without paging (free and standby; Linux: MemAvailable). 0: unknown.
uint64_t AvailablePhysicalMemory();
// The RAM the emulator has: the PC's (PhysicalMemory, so KYTY_SIMULATE_RAM_MB counts) less what other programs held when
// it started, i.e. what the system could give without paging then plus the emulator's own working set (taken at the
// first call, made before main; logged once). The memory savings of PCs short of RAM go by it, not by the PC's RAM
// (issue #228: a 24 GB PC whose other programs held 10 GB ran out of memory at a world load, its savings off). 0: unknown.
uint64_t EmulatorMemory();
// The memory the process can still commit and the commit limit (RAM and page file: Task Manager's "Committed"; Linux:
// CommitLimit less Committed_AS, and CommitLimit, which overcommit lets programs pass), in bytes. 0: unknown.
struct CommitState {
	uint64_t available = 0;
	uint64_t limit     = 0;
};
CommitState SystemCommit();
// KYTY_HEAP_TRIM=1 (off by default): the heap's free pages go back to the system on a thread of its
// own, checked every 30 s, after 64 MiB of growth since the last trim; the first 10 are told.
// Linux (glibc): malloc_trim (in Demon's Souls 130-150 MiB of its 1 GB of arenas were free chunks
// still in RAM). Windows 8.1 and later: HeapOptimizeResources on the process heap (the CRT's).
void StartHeapTrimmer();
// Diagnostics: the busy blocks of every heap of the process by size ("size<TAB>count<TAB>bytes" lines to `path`, the
// heaps locked while walked: a pause of a second or so), by size and first qword (<path>.sig.tsv: SPIR-V, a vtable as
// module+offset, zero, data) and the committed private allocations (<path>.vm.tsv, the heaps' marked); Linux (glibc):
// the arenas' totals and malloc_info's XML instead. false where the host cannot list them.
bool WriteHeapHistogram(const char* path);
// Diagnostics: the stacks of live heap blocks of the given sizes ("2848,5151,16-256"): StartAllocTrace hooks the process
// heap calls of every loaded module (their imports of RtlAllocateHeap/RtlFreeHeap/RtlReAllocateHeap) and records each
// such block from then on; WriteAllocTrace writes the live ones by stack ("count<TAB>bytes<TAB>size<TAB>frames" lines,
// a frame as module+offset). false where there is no such hook.
bool StartAllocTrace(const char* sizes);
bool WriteAllocTrace(const char* path);

// Windows' own figures for this process's video memory on the GPU with this LUID (Vulkan's deviceLUID, 8 bytes): in
// use (what Task Manager shows as its dedicated GPU memory) and the budget Windows gives the process (DXGI). NVIDIA's
// Vulkan heap usage counts what its system memory type holds as well. false where there are none (other systems).
bool OpenVideoMemoryAdapter(const uint8_t* luid);
bool QueryVideoMemory(uint64_t* usage, uint64_t* budget);
// The process's system memory the GPU uses (Task Manager's shared GPU memory).
bool QuerySharedGpuMemory(uint64_t* usage);

// The file's Authenticode signature is valid (no network: revocation not checked) and its signer is NVIDIA
// Corporation: NVIDIA's DLLs the emulator loads (Streamline for frame generation, DLSS). false elsewhere.
bool IsNvidiaSigned(const std::wstring& path);
// The same for AMD's (Advanced Micro Devices: the FidelityFX runtime, KYTY_FRAMEGEN_BACKEND=fsr).
bool IsAmdSigned(const std::wstring& path);

#if defined(_WIN32)
// With its output in files (run-windows.ps1's logs), the console window a launcher gave the process
// stays empty, and closing it would end the game: the process leaves it (it closes).
void ReleaseRedirectedConsole();

// Standard output written to a file: through a pipe a background thread empties into the file (a write to the
// redirected log blocked the writer ~6 ms in WriteFile: the GPU thread's per-slow-frame flush took 9% of its time,
// 10-06). DrainStdout, at the process's end, sends standard output to NUL and waits until what was written before
// reached the file.
void AsyncStdout();
void DrainStdout();
#endif

// Collects the unwind tables of the loaded images for SampleThread (call before sampling). Linux: nothing to
// collect; SampleThread falls back to the frame-pointer chain.
void PrepareSampling();

// Suspends the thread (a CurrentThreadHandle) and records pc and its callers (unwound with
// the tables of PrepareSampling, else the word at rsp and the frame-pointer chain) within its
// stack [stack_low, stack_high) into out[0..words). stack_high 0: the stack is unknown (a guest
// thread), the walk stays inside the committed region that holds rsp. Linux: no cross-thread sampler (false).
bool SampleThread(uint64_t handle, uint64_t stack_low, uint64_t stack_high, uint64_t* out, size_t words);
// A handle to another thread of this process for SampleThread and PinThreadHandleToCpuList (0 if
// it cannot be opened), and its release. Linux: ProcessThreads is empty, so it is never needed (0).
uint64_t OpenThreadForSampling(uint32_t thread_id);
void     CloseThreadForSampling(uint64_t handle);

#if defined(_WIN32)
// Base address and path of the module that contains the address.
bool ModuleOf(const void* address, uint64_t* base, char* path, size_t path_size);
#endif

} // namespace LocalPlatform
