// bar_crash.cpp — automatic crash capture. Windows release builds have no console, so a backtrace
// printed to stderr is lost. Write a minidump and a short report to the same directory as saves.
//
// Windows uses DbgHelp + SetUnhandledExceptionFilter. Linux has its own handler below (a fatal-signal
// handler printing backtrace_symbols_fd offsets for addr2line). Other platforms get an empty object, and
// main.cpp calls bar_install_crash_handler() only on Windows and Linux.
#ifdef _WIN32
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <windows.h>
#include <dbghelp.h>
#include "game/config.hpp"

namespace {
// Resolve the directory before a fault. No C++ filesystem access in the exception handler.
wchar_t g_crash_directory[32768] = L".";
std::atomic<long> g_handling_crash{0};
std::atomic<int> g_last_game_state{-1};
std::atomic<unsigned long long> g_last_state_tick{0};
std::atomic<unsigned long long> g_si_polls{0};

void report_crash(FILE* out, EXCEPTION_POINTERS* ep, const wchar_t* dump_path,
                  bool dump_written, DWORD dump_error) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const EXCEPTION_RECORD* const record = ep->ExceptionRecord;
    const DWORD64 address = reinterpret_cast<DWORD64>(record->ExceptionAddress);
    MEMORY_BASIC_INFORMATION mem{};
    wchar_t module[MAX_PATH] = L"";
    DWORD64 offset = 0;
    if (VirtualQuery(record->ExceptionAddress, &mem, sizeof(mem)) != 0 &&
        GetModuleFileNameW(static_cast<HMODULE>(mem.AllocationBase), module, MAX_PATH) != 0) {
        offset = address - reinterpret_cast<DWORD64>(mem.AllocationBase);
    }

    std::fwprintf(out, L"Beetle Adventure Racing Recomp crash: %04u-%02u-%02u %02u:%02u:%02u\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::fwprintf(out, L"exception=0x%08lX  address=0x%llX  pid=%lu  thread=%lu\n",
                  record->ExceptionCode, static_cast<unsigned long long>(address),
                  GetCurrentProcessId(), GetCurrentThreadId());
    if (module[0]) std::fwprintf(out, L"module=%ls+0x%llX\n", module,
                                  static_cast<unsigned long long>(offset));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const ULONG_PTR operation = record->ExceptionInformation[0];
        std::fwprintf(out, L"access=%ls  memory=0x%llX\n",
                      operation == 0 ? L"read" : operation == 1 ? L"write" :
                      operation == 8 ? L"execute" : L"unknown",
                      static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    const auto since = g_last_state_tick.load(std::memory_order_relaxed);
    std::fwprintf(out, L"game_state=%d  state_age_ms=%llu  si_polls=%llu\n",
                  g_last_game_state.load(std::memory_order_relaxed),
                  since ? GetTickCount64() - since : 0,
                  g_si_polls.load(std::memory_order_relaxed));
    std::fwprintf(out, L"dump=%ls  path=%ls\n",
                  dump_written ? L"written" : L"failed", dump_path);
    if (!dump_written) std::fwprintf(out, L"dump_error=%lu\n", dump_error);
    wchar_t running_exe[32768] = L"";
    GetModuleFileNameW(nullptr, running_exe, sizeof(running_exe) / sizeof(running_exe[0]));
    std::fwprintf(out, L"running_exe=%ls\n", running_exe);
    std::fwprintf(out, L"Keep the matching beetle-adventure-racing-recomp.pdb from the same build.\n");
    std::fflush(out);
}

static LONG WINAPI bar_crash_filter(EXCEPTION_POINTERS* ep) {
    if (g_handling_crash.exchange(1, std::memory_order_relaxed) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stem[256];
    std::swprintf(stem, sizeof(stem) / sizeof(stem[0]),
                  L"crash-%04u%02u%02u-%02u%02u%02u-%lu-%lu", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId(), GetCurrentThreadId());
    wchar_t dump_path[32768];
    wchar_t text_path[32768];
    std::swprintf(dump_path, sizeof(dump_path) / sizeof(dump_path[0]), L"%ls\\%ls.dmp", g_crash_directory, stem);
    std::swprintf(text_path, sizeof(text_path) / sizeof(text_path[0]), L"%ls\\%ls.txt", g_crash_directory, stem);

    DWORD dump_error = 0;
    bool dump_written = false;
    HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = ep;
        info.ClientPointers = FALSE;
        const MINIDUMP_TYPE flags = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithFullMemoryInfo);
        dump_written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                         flags, &info, nullptr, nullptr) != FALSE;
        if (!dump_written) dump_error = GetLastError();
        CloseHandle(file);
        if (!dump_written) DeleteFileW(dump_path);
    } else {
        dump_error = GetLastError();
    }
    if (FILE* report = _wfopen(text_path, L"w")) {
        report_crash(report, ep, dump_path, dump_written, dump_error);
        std::fclose(report);
    }
    report_crash(stderr, ep, dump_path, dump_written, dump_error);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

extern "C" void bar_crash_note_game_state(int state) {
    g_si_polls.fetch_add(1, std::memory_order_relaxed);
    if (g_last_game_state.exchange(state, std::memory_order_relaxed) != state)
        g_last_state_tick.store(GetTickCount64(), std::memory_order_relaxed);
}

void bar_install_crash_handler() {
    try {
        const std::wstring dir = bar::config::get_app_config_directory().wstring();
        if (dir.size() + 256 < sizeof(g_crash_directory) / sizeof(g_crash_directory[0]))
            std::wcscpy(g_crash_directory, dir.c_str());
    } catch (...) { /* a crash before config init still reports in the working directory */ }
    SetUnhandledExceptionFilter(bar_crash_filter);
}
#elif defined(__linux__)
// Linux: a fatal-signal handler printing the faulting address and a backtrace. backtrace_symbols_fd
// writes "exe(function+0x..)" or "exe(+0x..)"; the offsets resolve to function and line with
//     addr2line -f -C -e beetle-adventure-racing-recomp 0x<offset>
// (or llvm-symbolizer). Only async-signal-tolerant calls in the handler, then the default action runs.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <initializer_list>
#include <unistd.h>

static void bar_crash_signal(int sig, siginfo_t *info, void *) {
    char line[160];
    int n = std::snprintf(line, sizeof(line), "\n[beetle-adventure-racing-recomp] *** CRASH signal=%d (%s) addr=%p ***\n",
        sig, strsignal(sig), (info != nullptr) ? info->si_addr : nullptr);
    if (n > 0) { ssize_t ignored = write(STDERR_FILENO, line, size_t(n)); (void)ignored; }
    void *frames[48];
    int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

void bar_install_crash_handler() {
    struct sigaction action {};
    action.sa_sigaction = bar_crash_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE }) {
        sigaction(sig, &action, nullptr);
    }
}
#endif // _WIN32 / __linux__
