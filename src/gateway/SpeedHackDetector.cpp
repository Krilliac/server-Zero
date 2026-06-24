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
 * @file SpeedHackDetector.cpp
 * @brief Independent-clock speedhack detector (ring + ratio + hysteresis/cooldown).
 */

#include "SpeedHackDetector.h"
#include "Timer.h"   // getMSTimeDiff (wrap-safe ms delta)

#include <cmath>

SpeedHackDetector::SpeedHackDetector(const SpeedHackConfig& cfg)
    : m_cfg(cfg),
      m_ring(),
      m_head(0),
      m_count(0),
      m_validCount(0),
      m_sumClient(0.0),
      m_sumReal(0.0),
      m_havePrev(false),
      m_prevClient(0),
      m_prevReal(0),
      m_consecutiveHot(0),
      m_inCooldown(false),
      m_cooldownUntil(0)
{
    uint32 w = m_cfg.window ? m_cfg.window : 1;
    m_ring.resize(w);
}

void SpeedHackDetector::Reset()
{
    for (size_t i = 0; i < m_ring.size(); ++i)
    {
        m_ring[i] = Sample();
    }
    m_head = 0;
    m_count = 0;
    m_validCount = 0;
    m_sumClient = 0.0;
    m_sumReal = 0.0;
    m_havePrev = false;
    m_prevClient = 0;
    m_prevReal = 0;
    m_consecutiveHot = 0;
    m_inCooldown = false;
    m_cooldownUntil = 0;
}

SpeedHackDecision SpeedHackDetector::Feed(uint32 clientTime, uint32 recvTime)
{
    SpeedHackDecision dec;

    if (!m_cfg.enable || m_ring.empty())
    {
        return dec;
    }

    // Build the new sample's deltas against the immediately preceding sample.
    Sample s;
    s.clientTime = clientTime;
    s.recvTime   = recvTime;
    s.cd = 0;
    s.rd = 0;
    s.valid = false;

    if (m_havePrev)
    {
        uint32 cd = getMSTimeDiff(m_prevClient, clientTime);
        uint32 rd = getMSTimeDiff(m_prevReal,   recvTime);

        // Per-pair guards (applied before accumulation):
        //  - rd == 0: two packets in the same ms, no real-time information.
        //  - rd > maxGapMs: player paused/stopped; stale clientTime would skew.
        //  - cd > rd + maxGapMs: absurd single-step forward jump (teleport/desync).
        bool guarded = (rd == 0)
                    || (m_cfg.maxGapMs > 0 && rd > m_cfg.maxGapMs)
                    || (m_cfg.maxGapMs > 0 && cd > rd + m_cfg.maxGapMs);

        if (!guarded)
        {
            s.cd = cd;
            s.rd = rd;
            s.valid = true;
        }
    }

    // prev always advances (even for a discarded pair).
    m_prevClient = clientTime;
    m_prevReal   = recvTime;
    m_havePrev   = true;

    // Slide the ring: evict the oldest slot (subtract its contribution) and
    // write the new sample at the head.
    if (m_count == m_ring.size())
    {
        const Sample& old = m_ring[m_head];
        if (old.valid)
        {
            m_sumClient -= (double)old.cd;
            m_sumReal   -= (double)old.rd;
            if (m_validCount > 0) { --m_validCount; }
        }
    }
    else
    {
        ++m_count;
    }

    m_ring[m_head] = s;
    m_head = (m_head + 1) % m_ring.size();

    if (s.valid)
    {
        m_sumClient += (double)s.cd;
        m_sumReal   += (double)s.rd;
        ++m_validCount;
    }

    // Cooldown housekeeping: clear it once the neutral clock passes the deadline.
    // getMSTimeDiff(recvTime, cooldownUntil) is the remaining ms while recvTime is
    // before the deadline, and 0 once recvTime has reached/passed it (forward dir).
    if (m_inCooldown && getMSTimeDiff(recvTime, m_cooldownUntil) == 0)
    {
        m_inCooldown = false;
    }

    // Need a full window of at least minSamples valid pairs before evaluating.
    if (m_validCount < m_cfg.minSamples || m_sumReal <= 0.0)
    {
        return dec;
    }

    const double ratio = m_sumClient / m_sumReal;

    const double tripHigh = 1.0 + (double)m_cfg.tolerancePct / 100.0;
    const double clearLow = 1.0 + ((double)m_cfg.tolerancePct / 2.0) / 100.0;

    if (ratio >= tripHigh)
    {
        ++m_consecutiveHot;
    }
    else if (ratio < clearLow)
    {
        m_consecutiveHot = 0;
    }
    // Between clearLow and tripHigh: hysteresis band -> hold the counter.

    if (m_consecutiveHot >= m_cfg.sustainWindows && !m_inCooldown)
    {
        const double overPct = (ratio - 1.0) * 100.0;
        long sev = (long)(overPct + 0.5); // round
        if (sev < 1)   { sev = 1; }
        if (sev > 100) { sev = 100; }

        dec.fire     = true;
        dec.severity = (uint8)sev;
        dec.ratio    = ratio;

        m_consecutiveHot = 0;
        m_inCooldown = true;
        m_cooldownUntil = recvTime + m_cfg.cooldownMs;
    }

    return dec;
}
