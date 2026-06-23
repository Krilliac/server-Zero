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
 * @file test_gateway_acevent.cpp
 * @brief Standalone round-trip test for the GW_AC_EVENT payload.
 *
 * The GW_AC_EVENT payload (uint32 clientId, uint8 acType, uint8 severity,
 * string detail) is what ClientSocket::ReportAcViolation builds on the gateway
 * and GatewayIntake::handleAcEvent parses on the node. This test encodes a known
 * payload with a ByteBuffer and decodes it back, asserting every field
 * round-trips — pinning the field ORDER both ends must agree on. No socket / no
 * node needed; it exercises the wire contract only.
 */

#include "ByteBuffer.h"
#include "Cluster/GatewayProtocol.h"

#include <cstdio>
#include <string>

int main(int /*argc*/, char** /*argv*/)
{
    int failures = 0;

    // Sanity: the frame type must exist and the protocol version is bumped.
    if (GW_AC_EVENT != 9)
    {
        printf("[FAIL] GW_AC_EVENT should be 9, is %d\n", (int)GW_AC_EVENT);
        ++failures;
    }
    else
    {
        printf("[PASS] GW_AC_EVENT == 9\n");
    }

    if (GW_PROTOCOL_VERSION < 2)
    {
        printf("[FAIL] GW_PROTOCOL_VERSION should be >= 2 (wire vocabulary changed), is %u\n",
               GW_PROTOCOL_VERSION);
        ++failures;
    }
    else
    {
        printf("[PASS] GW_PROTOCOL_VERSION == %u\n", GW_PROTOCOL_VERSION);
    }

    // Known values to round-trip.
    const uint32 clientId = 0xDEADBEEFu;
    const uint8  acType   = 16;          // AC_VIOLATION_RATE
    const uint8  severity = 42;
    const std::string detail = "selftest-detail";

    // Encode exactly as ClientSocket::ReportAcViolation does.
    ByteBuffer payload;
    payload << uint32(clientId);
    payload << uint8(acType);
    payload << uint8(severity);
    payload << detail;

    // Decode exactly as GatewayIntake::handleAcEvent does.
    uint32 rClientId; payload >> rClientId;
    uint8  rAcType;   payload >> rAcType;
    uint8  rSeverity; payload >> rSeverity;
    std::string rDetail; payload >> rDetail;

    if (rClientId == clientId) { printf("[PASS] clientId round-trips\n"); }
    else { printf("[FAIL] clientId: got %u expected %u\n", rClientId, clientId); ++failures; }

    if (rAcType == acType) { printf("[PASS] acType round-trips\n"); }
    else { printf("[FAIL] acType: got %u expected %u\n", (uint32)rAcType, (uint32)acType); ++failures; }

    if (rSeverity == severity) { printf("[PASS] severity round-trips\n"); }
    else { printf("[FAIL] severity: got %u expected %u\n", (uint32)rSeverity, (uint32)severity); ++failures; }

    if (rDetail == detail) { printf("[PASS] detail round-trips\n"); }
    else { printf("[FAIL] detail: got '%s' expected '%s'\n", rDetail.c_str(), detail.c_str()); ++failures; }

    // Frame build/parse: wrap the payload in a full GW_AC_EVENT frame and confirm
    // the header (len + type) is laid out as the node's parseFrames expects.
    ByteBuffer frame;
    GatewayFrame::Build(frame, (uint8)GW_AC_EVENT, payload /* already consumed; size only used */);

    // Rebuild a fresh payload (the one above was consumed by the reads).
    ByteBuffer payload2;
    payload2 << uint32(clientId);
    payload2 << uint8(acType);
    payload2 << uint8(severity);
    payload2 << detail;

    ByteBuffer frame2;
    GatewayFrame::Build(frame2, (uint8)GW_AC_EVENT, payload2);

    if (frame2.size() == GatewayFrame::HEADER_SIZE + payload2.size())
    {
        printf("[PASS] frame size == header(%u) + payload(%zu)\n",
               GatewayFrame::HEADER_SIZE, payload2.size());
    }
    else
    {
        printf("[FAIL] frame size %zu != header(%u) + payload(%zu)\n",
               frame2.size(), GatewayFrame::HEADER_SIZE, payload2.size());
        ++failures;
    }

    // The 5th byte of the frame is the type.
    if (frame2.size() >= GatewayFrame::HEADER_SIZE && frame2.contents()[4] == (uint8)GW_AC_EVENT)
    {
        printf("[PASS] frame type byte == GW_AC_EVENT\n");
    }
    else
    {
        printf("[FAIL] frame type byte mismatch\n");
        ++failures;
    }

    if (failures == 0)
    {
        printf("ALL TESTS PASSED\n");
        return 0;
    }

    printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
