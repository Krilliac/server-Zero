/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

/**
 * @file Main.cpp
 * @brief Cluster gateway daemon entry point
 *
 * The gateway daemon is the cluster's front door. In this skeleton it only:
 * - parses the command line (-c <conf>)
 * - loads its configuration
 * - initializes logging
 * - opens the login and character databases
 * - prints an online banner
 * - idles until stdin reaches EOF (or a termination signal arrives)
 *
 * Networking, node mesh, and client multiplexing are added in later tasks.
 */

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "Config/Config.h"
#include "GitRevision.h"
#include "Log.h"
#include "SystemConfig.h"
#include "Util.h"

#include "ClientSocketMgr.h"
#include "ClientSocket.h"
#include "NodeRegistry.h"
#include "EdgeCheckConfig.h"
#include "SessionGuard.h"

#include <ace/Get_Opt.h>
#include <ace/INET_Addr.h>

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#define GATEWAY_CONFIG_NAME     "gateway.conf"
#define GATEWAY_CONFIG_LOCATION SYSCONFDIR GATEWAY_CONFIG_NAME

bool stopEvent = false;                                     ///< Setting it to true stops the gateway
uint8 exitCode = 0;                                         ///< Process exit code

DatabaseType LoginDatabase;                                 ///< Accessor to the realm/login database
DatabaseType CharacterDatabase;                             ///< Accessor to the character database

EdgeCheckConfig g_edgeCfg;                                  ///< Phase 2 edge-check tunables (loaded at boot)

bool StartDB();
void StopDB();
void HookSignals();
void UnhookSignals();

/// Print command line usage information
void usage(const char* prog)
{
    sLog.outString("Usage: \n %s [<options>]\n"
        "    -v, --version            print version and exit\n\r"
        "    -c config_file           use config_file as configuration file\n\r"
        , prog);
}

/// Put the global variable stopEvent to 'true' if a termination signal is caught
void OnSignal(int s)
{
    switch (s)
    {
        case SIGINT:
        case SIGTERM:
            stopEvent = true;
            break;
#ifdef _WIN32
        case SIGBREAK:
            stopEvent = true;
            break;
#endif
    }

    signal(s, OnSignal);
}

/// Gateway daemon entry point
extern int main(int argc, char** argv)
{
    ///- Command line parsing
    char const* cfg_file = GATEWAY_CONFIG_LOCATION;

    char const* options = ":c:";

    ACE_Get_Opt cmd_opts(argc, argv, options);
    cmd_opts.long_option("version", 'v');

    int option;
    while ((option = cmd_opts()) != EOF)
    {
        switch (option)
        {
            case 'c':
                cfg_file = cmd_opts.opt_arg();
                break;
            case 'v':
                printf("%s\n", GitRevision::GetProjectRevision());
                return 0;
            case ':':
                sLog.outError("Runtime-Error: -%c option requires an input argument", cmd_opts.opt_opt());
                usage(argv[0]);
                Log::WaitBeforeContinueIfNeed();
                return 1;
            default:
                sLog.outError("Runtime-Error: bad format of commandline arguments");
                usage(argv[0]);
                Log::WaitBeforeContinueIfNeed();
                return 1;
        }
    }

    ///- Load the configuration file
    if (!sConfig.SetSource(cfg_file))
    {
        // Try current folder as fallback if SYSCONFDIR path fails
        if (!sConfig.SetSource(GATEWAY_CONFIG_NAME))
        {
            sLog.outError("Could not find configuration file %s.", cfg_file);
            Log::WaitBeforeContinueIfNeed();
            return 1;
        }
        cfg_file = GATEWAY_CONFIG_NAME;
    }

    sLog.Initialize();

    sLog.outString("%s [cluster-gateway]", GitRevision::GetProjectRevision());
    sLog.outString("%s", GitRevision::GetFullRevision());
    sLog.outString("<Ctrl-C> to stop.\n");
    sLog.outString("Using configuration file %s.", cfg_file);

    DETAIL_LOG("Using ACE: %s", ACE_VERSION);

    /// Gateway PID file creation
    std::string pidfile = sConfig.GetStringDefault("PidFile", "");
    if (!pidfile.empty())
    {
        uint32 pid = CreatePIDFile(pidfile);
        if (!pid)
        {
            sLog.outError("Can not create PID file %s.\n", pidfile.c_str());
            Log::WaitBeforeContinueIfNeed();
            return 1;
        }

        sLog.outString("Daemon PID: %u\n", pid);
    }

    ///- Initialize the database connections
    if (!StartDB())
    {
        Log::WaitBeforeContinueIfNeed();
        return 1;
    }

    ///- Catch termination signals
    HookSignals();

    ///- Load anti-cheat edge-check config once at boot (Phase 2) and configure
    ///  the cross-connection SessionGuard from it. Load the independent-clock
    ///  speedhack config (Phase 3) into the process-wide SpeedHackConfig.
    g_edgeCfg.LoadFromConfig();
    sSessionGuard().Configure(g_edgeCfg);
    sLog.outString("gateway: AntiCheat enable=%u rate=%u protocol=%u session=%u (burst=%u global=%u/s flood=%u/s maxPayload=%u accts/ip=%u)",
        g_edgeCfg.enable ? 1u : 0u, g_edgeCfg.rateEnable ? 1u : 0u,
        g_edgeCfg.protocolEnable ? 1u : 0u, g_edgeCfg.sessionEnable ? 1u : 0u,
        g_edgeCfg.rateBurst, g_edgeCfg.globalOpcodesPerSec, g_edgeCfg.floodDisconnectPerSec,
        g_edgeCfg.maxPayloadBytes, g_edgeCfg.maxAccountsPerIp);
    ClientSocket::LoadSpeedConfig();

    ///- Start accepting inbound game-client connections
    uint16 gatewayPort = sConfig.GetIntDefault("GatewayPort", DEFAULT_WORLDSERVER_PORT);
    ACE_INET_Addr bind_addr(gatewayPort, "0.0.0.0");
    if (sClientSocketMgr->StartNetwork(bind_addr) == -1)
    {
        sLog.outError("Failed to start client network on port %u", gatewayPort);
        UnhookSignals();
        StopDB();
        Log::WaitBeforeContinueIfNeed();
        return 1;
    }

    ///- Build the backend node link pool from config (Phase 2: N nodes) and
    ///  start every link. Each link keeps retrying its connect on its own
    ///  thread; failure to connect is non-fatal (a node may not be up yet).
    ///  Fail-closed on an empty Gateway.Secret is enforced inside StartAll().
    sNodeRegistry().LoadFromConfig();
    sNodeRegistry().StartAll();

    sLog.outString();
    sLog.outString("==============================================");
    sLog.outString(" Gateway online (GatewayPort %u)", gatewayPort);
    sLog.outString("==============================================");
    sLog.outString();

    // server has started up successfully => enable async DB requests
    LoginDatabase.AllowAsyncTransactions();
    CharacterDatabase.AllowAsyncTransactions();

    ///- Idle loop: block on stdin and exit on EOF (or a termination signal)
    while (!stopEvent)
    {
        int c = getchar();
        if (c == EOF)
        {
            break;
        }
        // Ignore any other console input in this skeleton.
    }

    ///- Clean shutdown
    sNodeRegistry().StopAll();
    sClientSocketMgr->StopNetwork();
    UnhookSignals();
    StopDB();

    sLog.outString("Halting process...");
    return exitCode;
}

/// Initialize connections to the databases
bool StartDB()
{
    std::string loginstring = sConfig.GetStringDefault("LoginDatabaseInfo", "");
    if (loginstring.empty())
    {
        sLog.outError("Login database not specified (LoginDatabaseInfo)");
        return false;
    }

    sLog.outString("Login Database total connections: %i", 1 + 1);
    if (!LoginDatabase.Initialize(loginstring.c_str()))
    {
        sLog.outError("Can not connect to login database");
        return false;
    }

    std::string charstring = sConfig.GetStringDefault("CharacterDatabaseInfo", "");
    if (charstring.empty())
    {
        sLog.outError("Character database not specified (CharacterDatabaseInfo)");
        LoginDatabase.HaltDelayThread();
        return false;
    }

    sLog.outString("Character Database total connections: %i", 1 + 1);
    if (!CharacterDatabase.Initialize(charstring.c_str()))
    {
        sLog.outError("Can not connect to character database");
        LoginDatabase.HaltDelayThread();
        return false;
    }

    return true;
}

/// Close the database connections
void StopDB()
{
    CharacterDatabase.HaltDelayThread();
    LoginDatabase.HaltDelayThread();
}

/// Define hook 'OnSignal' for all termination signals
void HookSignals()
{
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
#ifdef _WIN32
    signal(SIGBREAK, OnSignal);
#endif
}

/// Unhook the signals before leaving
void UnhookSignals()
{
    signal(SIGINT, 0);
    signal(SIGTERM, 0);
#ifdef _WIN32
    signal(SIGBREAK, 0);
#endif
}
