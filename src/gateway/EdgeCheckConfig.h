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
 * @file EdgeCheckConfig.h
 * @brief Phase 2 gateway edge anti-cheat tunables, loaded once at boot.
 *
 * Holds every Gateway.AntiCheat.* knob with a sane, tolerant default. A single
 * instance is read at boot (EdgeCheckConfig::LoadFromConfig) and copied into
 * each ClientSocket's RateLimiter / ProtocolValidator and into SessionGuard, so
 * the hot packet path never touches sConfig.
 */

#ifndef GATEWAY_H_EDGECHECKCONFIG
#define GATEWAY_H_EDGECHECKCONFIG

#include "Common.h"

/**
 * @brief All Phase 2 edge-check tunables. Defaults are deliberately generous so
 *        a normal client at high latency never trips them.
 */
struct EdgeCheckConfig
{
    bool   enable;                 ///< master gate for the whole edge suite

    // --- rate / flood ---
    bool   rateEnable;             ///< per-opcode token-bucket limiter on/off
    uint32 rateBurst;              ///< per-opcode bucket capacity (burst allowance)
    double rateRefillPerSec;       ///< per-opcode tokens refilled per second
    uint32 globalOpcodesPerSec;    ///< report threshold: total packets/sec/conn
    uint32 floodDisconnectPerSec;  ///< egregious: total packets/sec/conn -> drop

    // --- protocol / opcode validation ---
    bool   protocolEnable;
    uint32 maxPayloadBytes;        ///< per-packet payload cap (header already caps at 10240)

    // --- session integrity ---
    bool   sessionEnable;
    bool   sessionOnePerAccount;   ///< one live connection per account cluster-wide
    uint32 maxAccountsPerIp;       ///< accounts-per-source-IP cap (0 = unlimited)
    bool   flagIpChange;           ///< report a mid-session source-IP change

    // --- severities (the 0..255 hint the node maps to a score weight) ---
    uint8  severityRate;
    uint8  severityProtocol;
    uint8  severitySession;

    EdgeCheckConfig()
        : enable(true),
          rateEnable(true),
          rateBurst(40),
          rateRefillPerSec(20.0),
          globalOpcodesPerSec(300),
          floodDisconnectPerSec(2000),
          protocolEnable(true),
          maxPayloadBytes(8192),
          sessionEnable(true),
          sessionOnePerAccount(true),
          maxAccountsPerIp(8),
          flagIpChange(true),
          severityRate(20),
          severityProtocol(30),
          severitySession(25)
    {
    }

    /// Read every key off sConfig, overwriting the defaults above. Call once at
    /// boot (after sConfig.SetSource succeeds).
    void LoadFromConfig();
};

#endif /* GATEWAY_H_EDGECHECKCONFIG */
