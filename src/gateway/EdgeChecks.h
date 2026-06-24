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
 * @file EdgeChecks.h
 * @brief Pure, testable per-connection gateway edge detectors (rate + protocol).
 *
 * No socket / ACE / game dependency: a TokenBucket, a per-opcode RateLimiter,
 * and a ProtocolValidator. All clock input is an explicit millisecond stamp so
 * the detectors are deterministic under test. The socket feeds them getMSTime()
 * at runtime.
 */

#ifndef GATEWAY_H_EDGECHECKS
#define GATEWAY_H_EDGECHECKS

#include "Common.h"
#include "EdgeCheckConfig.h"
#include "Timer.h"   // getMSTimeDiff (wrap-safe ms delta)

#include <map>

/**
 * @brief A classic token bucket. Refills lazily from the supplied clock.
 */
struct TokenBucket
{
    double tokens;   ///< current tokens (fractional ok)
    uint32 lastMs;   ///< last refill timestamp
    bool   primed;   ///< false until first Consume seeds lastMs

    TokenBucket() : tokens(0.0), lastMs(0), primed(false) {}

    void Init(uint32 capacity, double /*refillPerSec*/)
    {
        tokens = (double)capacity;
        lastMs = 0;
        primed = false;
    }

    /// Refill by elapsed time (clamped to capacity), then try to take one token.
    /// @return true if a token was available (allowed), false if empty (over).
    bool Consume(uint32 atMs, uint32 capacity, double refillPerSec)
    {
        if (!primed) { lastMs = atMs; primed = true; }
        if (atMs > lastMs && refillPerSec > 0.0)
        {
            double elapsedSec = (double)(atMs - lastMs) / 1000.0;
            tokens += elapsedSec * refillPerSec;
            if (tokens > (double)capacity) { tokens = (double)capacity; }
            lastMs = atMs;
        }
        else if (atMs > lastMs)
        {
            lastMs = atMs;
        }
        if (tokens >= 1.0)
        {
            tokens -= 1.0;
            return true;
        }
        return false;
    }
};

/**
 * @brief Per-connection rate limiter: per-opcode token buckets + a per-second
 *        global packet counter. One instance lives in each ClientSocket.
 */
class RateLimiter
{
    public:
        enum Result { OK, RATE_VIOLATION, FLOOD_DISCONNECT };

        RateLimiter()
            : m_burst(40), m_refill(20.0),
              m_globalPerSec(300), m_floodPerSec(2000),
              m_windowStartMs(0), m_windowCount(0), m_primed(false)
        {
        }

        void Init(const EdgeCheckConfig& cfg)
        {
            m_burst        = cfg.rateBurst;
            m_refill       = cfg.rateRefillPerSec;
            m_globalPerSec = cfg.globalOpcodesPerSec;
            m_floodPerSec  = cfg.floodDisconnectPerSec;
            m_buckets.clear();
            m_windowStartMs = 0;
            m_windowCount = 0;
            m_primed = false;
        }

        /// Account for one packet of @p opcode at @p atMs.
        /// Returns FLOOD_DISCONNECT if the global count in the current 1s window
        /// exceeds floodDisconnectPerSec; else RATE_VIOLATION if the per-opcode
        /// bucket is empty OR the global count exceeds globalOpcodesPerSec; else OK.
        Result OnOpcode(uint16 opcode, uint32 atMs)
        {
            // 1) global 1-second window counter (flood + global-rate gate).
            if (!m_primed) { m_windowStartMs = atMs; m_windowCount = 0; m_primed = true; }
            if (getMSTimeDiff(m_windowStartMs, atMs) >= 1000) // new window
            {
                m_windowStartMs = atMs;
                m_windowCount = 0;
            }
            ++m_windowCount;

            if (m_floodPerSec > 0 && m_windowCount > m_floodPerSec)
            {
                return FLOOD_DISCONNECT;
            }

            bool globalOver = (m_globalPerSec > 0 && m_windowCount > m_globalPerSec);

            // 2) per-opcode token bucket.
            TokenBucket& b = m_buckets[opcode];
            if (b.lastMs == 0 && !b.primed && b.tokens == 0.0)
            {
                b.Init(m_burst, m_refill);
            }
            bool bucketOk = b.Consume(atMs, m_burst, m_refill);

            if (!bucketOk || globalOver)
            {
                return RATE_VIOLATION;
            }
            return OK;
        }

    private:
        uint32 m_burst;
        double m_refill;
        uint32 m_globalPerSec;
        uint32 m_floodPerSec;

        uint32 m_windowStartMs;
        uint32 m_windowCount;
        bool   m_primed;

        std::map<uint16, TokenBucket> m_buckets;
};

/**
 * @brief Per-connection protocol/opcode validator (size + state).
 */
class ProtocolValidator
{
    public:
        enum Result { OK, OVERSIZE, ILLEGAL_STATE };

        ProtocolValidator() : m_enable(true), m_maxPayload(8192), m_strictPreWorld(false) {}

        void Init(const EdgeCheckConfig& cfg)
        {
            m_enable         = cfg.protocolEnable;
            m_maxPayload     = cfg.maxPayloadBytes;
            m_strictPreWorld = cfg.protocolStrictPreWorld;
        }

        /// @param worldEntered true once CMSG_PLAYER_LOGIN has been forwarded.
        Result Check(uint16 opcode, uint32 payloadLen, bool worldEntered) const;

    private:
        bool   m_enable;
        uint32 m_maxPayload;
        bool   m_strictPreWorld;  ///< only enforce the pre-world allowlist when true

        /// True if @p opcode is legitimately sent between auth and world entry.
        static bool IsPreWorldAllowed(uint16 opcode);
};

#endif /* GATEWAY_H_EDGECHECKS */
