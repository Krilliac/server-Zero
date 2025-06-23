#include "CrashHandler.h"
#include "BreadcrumbLogger.h"
#include "Log.h"
#include <csignal>
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
#include <Windows.h>
#include <DbgHelp.h>
#else
#include <execinfo.h>
#include <unistd.h>
#endif

void CrashHandler::Initialize()
{
    // Register signal handlers
    signal(SIGSEGV, HandleCrash);  // Segmentation fault
    signal(SIGABRT, HandleCrash);  // Abort
    signal(SIGFPE, HandleCrash);   // Floating point exception
    signal(SIGILL, HandleCrash);   // Illegal instruction
}

void CrashHandler::HandleCrash(int signal)
{
    // Log crash signal
    const char* signalName = "Unknown";
    switch (signal) {
        case SIGSEGV: signalName = "SIGSEGV"; break;
        case SIGABRT: signalName = "SIGABRT"; break;
        case SIGFPE:  signalName = "SIGFPE"; break;
        case SIGILL:  signalName = "SIGILL"; break;
    }
    
    sLog.outError("CRASH: Signal %d (%s) received", signal, signalName);
    
    // Generate crash dump
    GenerateCrashDump();
    
    // Dump breadcrumbs to file
    BreadcrumbLogger::DumpToFile("logs/crash_breadcrumbs.log");
    
    // Attempt to shutdown cleanly
    sWorld.StopNow(5); // 5 second shutdown
    
    // Re-raise signal to exit
    signal(signal, SIG_DFL);
    raise(signal);
}

void CrashHandler::GenerateCrashDump()
{
    const std::string filename = "logs/crashdump_" + 
        std::to_string(std::time(nullptr)) + ".dmp";
    
#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename.c_str(), GENERIC_WRITE, 0, NULL, 
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info;
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = nullptr;
        info.ClientPointers = FALSE;
        
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, 
                          MiniDumpNormal, &info, NULL, NULL);
        CloseHandle(hFile);
    }
#else
    void* callstack[128];
    int frames = backtrace(callstack, 128);
    FILE* file = fopen(filename.c_str(), "w");
    if (file) {
        backtrace_symbols_fd(callstack, frames, fileno(file));
        fclose(file);
    }
#endif
}
