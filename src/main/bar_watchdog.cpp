// bar_watchdog.cpp -- catches the AppHang that bar_crash.cpp structurally cannot.
//
// WHY. Windows Error Reporting has six `AppHangB1` records for this exe (7 Sep -> 19 Sep 2026), all
// carrying the *same* hang-stack hash `170f3bf0b8952de987768585f9009d4b` across five different
// builds, so the pump thread stops at one specific place. None of the six has a `.hdmp`, so no
// thread stacks were ever captured and there is nothing to symbolize after the fact. The port's own
// handler in bar_crash.cpp is an *unhandled-exception* filter -- a hang raises no exception, so it
// never fires. See docs/KNOWN_ISSUES.md for the table and what has been ruled out.
//
// WHAT. A monitor thread watches a per-frame counter written by update_gfx. update_gfx is the
// callback that owns the window and, per docs/technical/05-runtime-host.md ("SDL event ownership"),
// is the sole place SDL's queue is drained -- so if it stops advancing, the window stops answering
// Windows and an AppHang follows. When the counter has been stale for BAR_WATCHDOG_SECS (default 5)
// the monitor walks every thread in the process and writes a symbolized report to
//     %LOCALAPPDATA%\beetle-adventure-racing-recomp\hang-report-<timestamp>.txt
// (and to stderr, which only goes somewhere with BAR_DBG_UI=1). The stall does not have to be fatal:
// if the counter starts moving again the episode is closed and the watchdog re-arms, so a recovered
// stall is reported too. That is deliberate -- a near-miss has the same stack as the fatal one.
//
// SUSPENDED-THREAD SAFETY. Walking another thread's stack needs it suspended, or the walk reads a
// stack that is being mutated. But allocating while a thread is suspended can deadlock on the CRT
// heap lock if that thread happened to hold it -- and the process is already wedged, so a watchdog
// that wedges too would produce nothing. Three rules keep that from happening:
//   1. Everything that allocates happens before any thread is suspended: the thread-id snapshot, the
//      DbgHelp init, and a dummy self-walk that forces DbgHelp's lazy allocations to happen up front.
//   2. Exactly one thread is suspended at a time, and only raw frame addresses are collected while
//      it is -- into storage reserved in step 1.
//   3. Symbolization (SymFromAddr / SymGetLineFromAddr64) runs after every thread has been resumed.
// Symbol names need beetle-adventure-racing-recomp.pdb next to the exe, as bar_crash.cpp does; with
// no PDB the report still gives module+offset, which is enough to resolve offline.
//
// KNOWN FALSE POSITIVE. A legitimate stall longer than the threshold -- first-race shader/pipeline
// creation is the plausible one -- produces a report. That is cheap (a text file) and is itself worth
// knowing, so it is not suppressed. Raise BAR_WATCHDOG_SECS if it becomes noise.
//
// Switches: BAR_WATCHDOG=0 disables it entirely; BAR_WATCHDOG_SECS=<n> sets the threshold.

#include "bar_watchdog.h"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include "game/config.hpp"   // bar::config::get_app_config_directory (where the report is written)

namespace {

constexpr int kMaxThreads  = 192;   // generous: the port runs well under 40
constexpr int kMaxFrames   = 62;    // per thread
constexpr int kMaxReports  = 8;     // per process run, so a pathological loop cannot spew files

std::atomic<uint64_t> g_beat{0};          // written by update_gfx, read by the monitor
std::atomic<uint64_t> g_game_beat{0};     // written by the game's controller poll
std::atomic<int>      g_game_state{-1};
std::atomic<DWORD>    g_game_tid{0};
std::atomic<bool>     g_installed{false};
std::string           g_report_dir;       // resolved at install time; the monitor must not touch fs later
char                  g_running_exe[MAX_PATH] = {};
std::atomic<DWORD>    g_pump_tid{0};      // set by the first heartbeat: the thread we are watching
double                g_threshold_secs = 5.0;

// One thread's captured state. Fixed-size so filling it needs no allocation.
struct ThreadTrace {
    DWORD   tid;
    int     frame_count;
    bool    walked;
    DWORD64 frames[kMaxFrames];
    wchar_t name[64];
};

using GetThreadDescription_t = HRESULT (WINAPI*)(HANDLE, PWSTR*);
GetThreadDescription_t g_get_thread_description = nullptr;

// Collect this process's thread ids. Allocates (the snapshot does), so it runs before any suspend.
int snapshot_thread_ids(DWORD* out, int cap) {
    const DWORD self_pid = GetCurrentProcessId();
    const DWORD self_tid = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    int n = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID))
                continue;
            if (te.th32OwnerProcessID != self_pid) continue;
            if (te.th32ThreadID == self_tid) continue;   // never suspend the watchdog itself
            if (n < cap) out[n++] = te.th32ThreadID;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

// Walk one suspended thread. Collects raw addresses only -- no symbolization, no allocation.
int walk_suspended(HANDLE thread, CONTEXT& ctx, DWORD64* frames, int cap) {
    STACKFRAME64 sf{};
    sf.AddrPC.Offset    = ctx.Rip; sf.AddrPC.Mode    = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;

    const HANDLE proc = GetCurrentProcess();
    int n = 0;
    while (n < cap) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &sf, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (sf.AddrPC.Offset == 0) break;
        frames[n++] = sf.AddrPC.Offset;
    }
    return n;
}

// Resolve one address to "name +0xdisp (file:line)" or "module+0x..". Runs only after every thread
// has been resumed, so it is free to allocate.
void print_frame(std::FILE* f, DWORD64 addr, int index) {
    const HANDLE proc = GetCurrentProcess();

    char buf[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    std::memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 511;

    DWORD64 disp = 0;
    if (SymFromAddr(proc, addr, &disp, sym)) {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_disp = 0;
        if (SymGetLineFromAddr64(proc, addr, &line_disp, &line))
            std::fprintf(f, "  #%2d %s +0x%llx   (%s:%lu)\n", index, sym->Name,
                         (unsigned long long)disp, line.FileName, line.LineNumber);
        else
            std::fprintf(f, "  #%2d %s +0x%llx\n", index, sym->Name, (unsigned long long)disp);
        return;
    }

    // No symbol: module + offset still resolves offline against the shipped PDB.
    const DWORD64 base = SymGetModuleBase64(proc, addr);
    char mod[MAX_PATH] = {};
    if (base != 0 && GetModuleFileNameA(reinterpret_cast<HMODULE>(base), mod, MAX_PATH) != 0) {
        const char* leaf = std::strrchr(mod, '\\');
        std::fprintf(f, "  #%2d %s+0x%llx\n", index, leaf ? leaf + 1 : mod,
                     (unsigned long long)(addr - base));
    } else {
        std::fprintf(f, "  #%2d 0x%llx\n", index, (unsigned long long)addr);
    }
}

void write_report(std::FILE* f, ThreadTrace* traces, int count, double stalled_secs,
                  const char* stalled_loop) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(f, "[beetle-adventure-racing-recomp] *** HANG: %s has not advanced for %.1fs ***\n",
                 stalled_loop, stalled_secs);
    std::fprintf(f, "time=%04u-%02u-%02u %02u:%02u:%02u  gfx_frames=%llu  game_polls=%llu  game_state=%d  pump_tid=%lu  game_tid=%lu  threads=%d\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 (unsigned long long)g_beat.load(std::memory_order_relaxed),
                 (unsigned long long)g_game_beat.load(std::memory_order_relaxed),
                 g_game_state.load(std::memory_order_relaxed),
                 (unsigned long)g_pump_tid.load(std::memory_order_relaxed),
                 (unsigned long)g_game_tid.load(std::memory_order_relaxed), count);
    std::fprintf(f, "running_exe=%s\nreport_dir=%s\n\n", g_running_exe, g_report_dir.c_str());

    for (int i = 0; i < count; i++) {
        const ThreadTrace& t = traces[i];
        std::fprintf(f, "--- thread %lu%s%s%s%ls%s\n",
                     (unsigned long)t.tid,
                     (t.tid == g_pump_tid.load(std::memory_order_relaxed)) ? "  [SDL PUMP]" : "",
                     (t.tid == g_game_tid.load(std::memory_order_relaxed)) ? "  [GAME POLL]" : "",
                     (t.name[0] != L'\0') ? "  \"" : "",
                     (t.name[0] != L'\0') ? t.name : L"",
                     (t.name[0] != L'\0') ? "\"" : "");
        if (!t.walked) {
            std::fprintf(f, "  (could not open or suspend this thread)\n\n");
            continue;
        }
        for (int j = 0; j < t.frame_count; j++) print_frame(f, t.frames[j], j);
        if (t.frame_count == 0) std::fprintf(f, "  (no frames)\n");
        std::fprintf(f, "\n");
    }
    std::fflush(f);
}

void capture(double stalled_secs, const char* stalled_loop, int report_index) {
    const HANDLE proc = GetCurrentProcess();
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char leaf[128];
    std::snprintf(leaf, sizeof(leaf), "hang-report-%04u%02u%02u-%02u%02u%02u-%d.txt",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, report_index);
    const std::string path = g_report_dir.empty() ? std::string(leaf) : (g_report_dir + "\\" + leaf);
    // Leave a useful marker even if stack capture itself blocks on a damaged runtime lock.
    if (std::FILE* f = std::fopen(path.c_str(), "w")) {
        std::fprintf(f, "Hang detected: %s stalled %.1fs; capturing thread stacks.\nrunning_exe=%s\n",
                     stalled_loop, stalled_secs, g_running_exe);
        std::fclose(f);
    }

    // --- Everything that allocates happens here, BEFORE the first suspend (rule 1). ---
    static std::vector<ThreadTrace> traces;   // reused across captures; sized once
    traces.resize(kMaxThreads);

    DWORD tids[kMaxThreads];
    const int count = snapshot_thread_ids(tids, kMaxThreads);
    if (count == 0) return;

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    // Force DbgHelp's lazy per-module allocations to happen now, on our own live stack, so the
    // per-thread walks below cannot be the first caller to allocate while something is suspended.
    {
        CONTEXT self{};
        self.ContextFlags = CONTEXT_FULL;
        RtlCaptureContext(&self);
        DWORD64 scratch[kMaxFrames];
        walk_suspended(GetCurrentThread(), self, scratch, kMaxFrames);
    }

    // --- Suspend / walk / resume, one thread at a time (rules 2 and 3). ---
    for (int i = 0; i < count; i++) {
        ThreadTrace& t = traces[i];
        t.tid = tids[i];
        t.frame_count = 0;
        t.walked = false;
        t.name[0] = L'\0';

        const HANDLE th = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[i]);
        if (th == nullptr) continue;

        if (g_get_thread_description != nullptr) {
            PWSTR desc = nullptr;
            if (SUCCEEDED(g_get_thread_description(th, &desc)) && desc != nullptr) {
                if (desc[0] != L'\0') {
                    wcsncpy_s(t.name, desc, _TRUNCATE);
                }
                LocalFree(desc);
            }
        }

        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx)) {
                t.frame_count = walk_suspended(th, ctx, t.frames, kMaxFrames);
                t.walked = true;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }

    // --- Symbolize and write, with nothing suspended. ---
    if (std::FILE* f = std::fopen(path.c_str(), "w")) {
        write_report(f, traces.data(), count, stalled_secs, stalled_loop);
        std::fclose(f);
        std::fprintf(stderr, "[beetle-adventure-racing-recomp] hang report written to %s\n", path.c_str());
    }
    write_report(stderr, traces.data(), count, stalled_secs, stalled_loop);

    // The text is complete before the dump: a stuck dump writer still leaves the stack report.
    std::string dump_path = path;
    dump_path.replace(dump_path.size() - 4, 4, ".dmp");
    if (HANDLE file = CreateFileA(dump_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        file != INVALID_HANDLE_VALUE) {
        const MINIDUMP_TYPE flags = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithFullMemoryInfo);
        const bool dumped = MiniDumpWriteDump(proc, GetCurrentProcessId(), file,
                                              flags, nullptr, nullptr, nullptr) != FALSE;
        const DWORD error = dumped ? 0 : GetLastError();
        CloseHandle(file);
        if (!dumped) DeleteFileA(dump_path.c_str());
        if (std::FILE* report = std::fopen(path.c_str(), "a")) {
            std::fprintf(report, "dump=%s  error=%lu  path=%s\n",
                         dumped ? "written" : "failed", (unsigned long)error, dump_path.c_str());
            std::fclose(report);
        }
    }

    SymCleanup(proc);
}

void monitor_loop() {
    using clock = std::chrono::steady_clock;

    uint64_t last_beat = g_beat.load(std::memory_order_relaxed);
    uint64_t last_game_beat = g_game_beat.load(std::memory_order_relaxed);
    auto last_change = clock::now();
    auto last_game_change = last_change;
    bool started = false;                // do not arm until the first frame has run
    bool reported = false;               // one report per pump stall episode
    bool game_reported = false;
    bool racing = false;
    bool capture_key_down = false;
    int      report_count = 0;

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const auto now = clock::now();
        const uint64_t beat = g_beat.load(std::memory_order_relaxed);
        if (beat != last_beat) {
            last_beat   = beat;
            last_change = now;
            started     = true;
            reported    = false;
        }

        const int state = g_game_state.load(std::memory_order_relaxed);
        const uint64_t game_beat = g_game_beat.load(std::memory_order_relaxed);
        if (state != 5) {
            racing = false;
            game_reported = false;
        } else if (!racing || game_beat != last_game_beat) {
            racing = true;
            last_game_change = now;
            game_reported = false;
        }
        last_game_beat = game_beat;

        // F12 takes the same all-thread snapshot without pausing or attaching a debugger.
        // It also covers a frozen picture when both monitored loops still make progress.
        const bool key_down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        DWORD foreground_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
        const bool manual_capture = key_down && !capture_key_down && foreground_pid == GetCurrentProcessId();
        capture_key_down = key_down;
        if (report_count >= kMaxReports) continue;
        if (manual_capture) {
            capture(0.0, "manual F12 snapshot", ++report_count);
            continue;
        }
        const double pump_stalled = std::chrono::duration<double>(now - last_change).count();
        const double game_stalled = std::chrono::duration<double>(now - last_game_change).count();
        const bool pump_hung = started && !reported && pump_stalled >= g_threshold_secs;
        // Leave longer for track loading and paused menus than for the ordinary SDL pump.
        const double game_timeout = g_threshold_secs > 10.0 ? g_threshold_secs : 10.0;
        const bool game_hung = racing && !game_reported && game_stalled >= game_timeout;
        if (pump_hung || game_hung) {
            if (pump_hung) reported = true;
            if (game_hung) game_reported = true;
            capture(pump_hung ? pump_stalled : game_stalled,
                    pump_hung ? "SDL pump" : "game controller poll (race)", ++report_count);
        }
    }
}

} // namespace

namespace bar::watchdog {

void install() {
    if (g_installed.exchange(true)) return;   // once per process

    if (const char* off = std::getenv("BAR_WATCHDOG"); off != nullptr && std::strcmp(off, "0") == 0) {
        return;
    }
    if (const char* secs = std::getenv("BAR_WATCHDOG_SECS"); secs != nullptr) {
        const double v = std::atof(secs);
        if (v >= 1.0) g_threshold_secs = v;
    }

    // Resolve the report directory once, on the caller's thread. The monitor must not touch the
    // filesystem layer later: during a hang, anything the wedged thread might hold is off limits.
    try {
        g_report_dir = bar::config::get_app_config_directory().string();
    } catch (...) {
        g_report_dir.clear();   // fall back to the working directory
    }
    GetModuleFileNameA(nullptr, g_running_exe, MAX_PATH);

    // Thread names make the report readable. Win10 1607+; absent is not fatal.
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        g_get_thread_description = reinterpret_cast<GetThreadDescription_t>(
            reinterpret_cast<void*>(GetProcAddress(k32, "GetThreadDescription")));
    }

    std::thread(monitor_loop).detach();
}

void heartbeat() {
    if (g_pump_tid.load(std::memory_order_relaxed) == 0)
        g_pump_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const uint64_t beat = g_beat.fetch_add(1, std::memory_order_relaxed) + 1;

    // Self-test. BAR_WATCHDOG_SELFTEST=<secs> wedges the pump thread once, on frame 300 (a few
    // seconds in, at the launcher), to prove the detector fires and the report symbolizes. Without
    // it the watchdog is only exercised by the bug it exists to catch, which is exactly the thing
    // that must not be discovered to be broken after the fact. Zero cost when unset: the env read
    // is a function-local static, so it happens once.
    static const double selftest_secs = [] {
        const char* v = std::getenv("BAR_WATCHDOG_SELFTEST");
        return (v != nullptr) ? std::atof(v) : 0.0;
    }();
    if (selftest_secs > 0.0 && beat == 300) {
        std::fprintf(stderr, "[beetle-adventure-racing-recomp] watchdog self-test: wedging the pump for %.1fs\n",
                     selftest_secs);
        Sleep(static_cast<DWORD>(selftest_secs * 1000.0));
    }
}

void game_heartbeat(int game_state) {
    g_game_state.store(game_state, std::memory_order_relaxed);
    g_game_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_game_beat.fetch_add(1, std::memory_order_relaxed);
}

} // namespace bar::watchdog

#else  // !_WIN32

// The six recorded hangs are all Windows AppHangs, and the stack walk here is DbgHelp-specific.
// Linux keeps its fatal-signal backtrace in bar_crash.cpp; a watchdog there would need its own
// mechanism, so this is deliberately a no-op rather than a half-working port.
namespace bar::watchdog {
void install() {}
void heartbeat() {}
void game_heartbeat(int) {}
} // namespace bar::watchdog

#endif // _WIN32
