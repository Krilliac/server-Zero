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
 * @file SpeedHackDetector.h
 * @brief Phase 3 independent-clock speedhack detector (pure, unit-testable).
 *
 * The gateway timestamps each watched movement packet on arrival with a neutral
 * monotonic clock the client cannot influence (getMSTime), and over a sliding
 * window compares the client's self-reported MovementInfo.time deltas against the
 * gateway's own receive cadence. A sustained ratio (client ms / real ms) above
 * tolerance = a fast clock = speed/time hack. No ACE / socket / game deps: the
 * caller passes recvTime in, so the detector is fully deterministic under test.
 */

#ifndef GATEWAY_H_SPEEDHACKDETECTOR
#define GATEWAY_H_SPEEDHACKDETECTOR

#include "Common.h"   // uint32 etc. (shared)

#include <vector>

/// All thresholds resolved from config once at startup (gateway Main.cpp).
struct SpeedHackConfig
{
    bool   enable         = false;
    uint32 window         = 20;
    uint32 minSamples     = 12;
    uint32 tolerancePct   = 30;
    uint32 sustainWindows = 3;
    uint32 maxGapMs       = 3000;
    uint32 cooldownMs     = 10000;
};

/// Result of feeding one movement sample.
struct SpeedHackDecision
{
    bool   fire     = false;  ///< true => caller should ReportAcViolation
    uint8  severity = 0;      ///< 1..100 (only meaningful if fire)
    double ratio    = 0.0;    ///< measured client/real ratio (for the detail string)
};

/// Pure, single-client, single-threaded detector. No ACE / socket / game deps.
/// The caller (ClientSocket) owns one instance and feeds it (clientTime, recvTime)
/// for each watched move packet; getMSTime() supplies recvTime on the real path,
/// but the class takes recvTime as a parameter so tests inject synthetic clocks.
class SpeedHackDetector
{
    public:
        explicit SpeedHackDetector(const SpeedHackConfig& cfg);

        /// Feed one sample. Returns a decision (fire/severity/ratio).
        SpeedHackDecision Feed(uint32 clientTime, uint32 recvTime);

        void Reset();   ///< clear window + counters (e.g. on re-home)

    private:
        struct Sample { uint32 clientTime; uint32 recvTime; uint32 cd; uint32 rd; bool valid; };

        const SpeedHackConfig& m_cfg;
        std::vector<Sample> m_ring;       ///< sized to cfg.window
        size_t  m_head;                   ///< next write index
        size_t  m_count;                  ///< filled slots (<= window)
        uint32  m_validCount;             ///< number of valid (accumulated) pairs in the ring
        double  m_sumClient;              ///< running sum of valid cd
        double  m_sumReal;                ///< running sum of valid rd
        bool    m_havePrev;
        uint32  m_prevClient;
        uint32  m_prevReal;
        uint32  m_consecutiveHot;         ///< sustained-window counter
        bool    m_inCooldown;
        uint32  m_cooldownUntil;          ///< getMSTime() value (recvTime domain)
};

#endif /* GATEWAY_H_SPEEDHACKDETECTOR */
