#pragma once

#include <psapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "Psapi.lib")

std::atomic_int diagnosticLogLevel = 0;

inline uint64_t FileTimeTicks(const FILETIME& value)
{
    return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

inline bool ReadCpuTime(bool process, uint64_t& ticks)
{
    FILETIME creation, exit, kernel, user;
    const BOOL ok = process
        ? GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)
        : GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
    if (ok)
        ticks = FileTimeTicks(kernel) + FileTimeTicks(user);
    return ok != FALSE;
}

class WorkerDiagnostics
{
public:
    WorkerDiagnostics(const char* name, bool reportProcess)
        : m_name(name), m_reportProcess(reportProcess)
    {
        m_threadValid = ReadCpuTime(false, m_threadCpu);
        m_processValid = ReadCpuTime(true, m_processCpu);
        LogMessage(std::string("[worker] ") + m_name + " entered");
    }

    ~WorkerDiagnostics()
    {
        report(true);
        LogMessage(std::string("[worker] ") + m_name + " exited");
    }

    void report(bool final = false)
    {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - m_last).count();
        if (!final && seconds < 2.0)
            return;

        uint64_t threadCpu = 0, processCpu = 0;
        const bool threadValid = ReadCpuTime(false, threadCpu);
        const bool processValid = ReadCpuTime(true, processCpu);
        if (diagnosticLogLevel.load() >= 1 && seconds > 0)
        {
            char line[384];
            const double threadMs = threadValid && m_threadValid
                ? (threadCpu - m_threadCpu) / 10000.0 : -1;
            snprintf(line, sizeof(line),
                "[cpu] worker=%s elapsed=%.3fs cpu=%.1fms cores=%.3f final=%d",
                m_name, seconds, threadMs, threadMs < 0 ? -1 : threadMs / (seconds * 1000), final);
            LogMessage(line);

            if (m_reportProcess)
            {
                DWORD handles = 0, threads = 0;
                GetProcessHandleCount(GetCurrentProcess(), &handles);
                PROCESS_MEMORY_COUNTERS_EX memory = {};
                memory.cb = sizeof(memory);
                GetProcessMemoryInfo(GetCurrentProcess(),
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
                const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snapshot != INVALID_HANDLE_VALUE)
                {
                    PROCESSENTRY32 entry = {};
                    entry.dwSize = sizeof(entry);
                    if (Process32First(snapshot, &entry))
                        do
                        {
                            if (entry.th32ProcessID == GetCurrentProcessId())
                            {
                                threads = entry.cntThreads;
                                break;
                            }
                        } while (Process32Next(snapshot, &entry));
                    CloseHandle(snapshot);
                }
                const double processMs = processValid && m_processValid
                    ? (processCpu - m_processCpu) / 10000.0 : -1;
                snprintf(line, sizeof(line),
                    "[process] elapsed=%.3fs cpu=%.1fms cores=%.3f threads=%lu handles=%lu privateBytes=%llu",
                    seconds, processMs, processMs < 0 ? -1 : processMs / (seconds * 1000),
                    threads, handles, static_cast<unsigned long long>(memory.PrivateUsage));
                LogMessage(line);
            }
        }
        m_last = now;
        m_threadCpu = threadCpu;
        m_processCpu = processCpu;
        m_threadValid = threadValid;
        m_processValid = processValid;
    }

private:
    const char* m_name;
    bool m_reportProcess;
    bool m_threadValid = false, m_processValid = false;
    uint64_t m_threadCpu = 0, m_processCpu = 0;
    std::chrono::steady_clock::time_point m_last = std::chrono::steady_clock::now();
};
