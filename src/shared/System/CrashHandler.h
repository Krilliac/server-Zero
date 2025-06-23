#pragma once

class CrashHandler
{
public:
    // Initializes crash signal handlers (call once at startup)
    static void Initialize();

    // Generates a crash dump (called from signal handler or manually)
    static void GenerateCrashDump();

private:
    // Signal handler for crashes (SIGSEGV, SIGABRT, etc)
    static void HandleCrash(int signal);
};
