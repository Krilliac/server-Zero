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
 * @file EdgeCheckConfig.cpp
 * @brief Loads the Gateway.AntiCheat.* tunables from sConfig at boot.
 */

#include "EdgeCheckConfig.h"
#include "Config/Config.h"

void EdgeCheckConfig::LoadFromConfig()
{
    enable                = sConfig.GetBoolDefault("Gateway.AntiCheat.Enable", true);

    rateEnable            = sConfig.GetBoolDefault("Gateway.AntiCheat.Rate.Enable", true);
    rateBurst             = (uint32)sConfig.GetIntDefault("Gateway.AntiCheat.Rate.Burst", 40);
    rateRefillPerSec      = (double)sConfig.GetFloatDefault("Gateway.AntiCheat.Rate.RefillPerSec", 20.0f);
    globalOpcodesPerSec   = (uint32)sConfig.GetIntDefault("Gateway.AntiCheat.Rate.GlobalPerSec", 300);
    floodDisconnectPerSec = (uint32)sConfig.GetIntDefault("Gateway.AntiCheat.Rate.FloodDisconnectPerSec", 2000);

    protocolEnable        = sConfig.GetBoolDefault("Gateway.AntiCheat.Protocol.Enable", true);
    maxPayloadBytes       = (uint32)sConfig.GetIntDefault("Gateway.AntiCheat.Protocol.MaxPayloadBytes", 8192);

    sessionEnable         = sConfig.GetBoolDefault("Gateway.AntiCheat.Session.Enable", true);
    sessionOnePerAccount  = sConfig.GetBoolDefault("Gateway.AntiCheat.Session.OnePerAccount", true);
    maxAccountsPerIp      = (uint32)sConfig.GetIntDefault("Gateway.AntiCheat.Session.MaxAccountsPerIp", 8);
    flagIpChange          = sConfig.GetBoolDefault("Gateway.AntiCheat.Session.FlagIpChange", true);

    severityRate          = (uint8)sConfig.GetIntDefault("Gateway.AntiCheat.Severity.Rate", 20);
    severityProtocol      = (uint8)sConfig.GetIntDefault("Gateway.AntiCheat.Severity.Protocol", 30);
    severitySession       = (uint8)sConfig.GetIntDefault("Gateway.AntiCheat.Severity.Session", 25);
}
