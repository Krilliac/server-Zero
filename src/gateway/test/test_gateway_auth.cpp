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
 * @file test_gateway_auth.cpp
 * @brief Standalone self-test for GatewayAuth::ValidateDigest.
 *
 * Independently recomputes the CMSG_AUTH_SESSION digest with the same SHA-1
 * field order and asserts ValidateDigest accepts the correct digest and
 * rejects a single-byte-flipped one. This pins the byte-assembly ORDER, which
 * is the security-critical invariant.
 */

#include "GatewayAuth.h"
#include "Auth/Sha1.h"
#include "Auth/BigNumber.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

int main(int /*argc*/, char** /*argv*/)
{
    int failures = 0;

    const std::string account = "TEST";
    const uint32 clientSeed = 0x11223344u;
    const uint32 serverSeed = 0xAABBCCDDu;

    // A fixed 40-byte session key.
    uint8 keyBytes[40];
    for (int i = 0; i < 40; ++i)
    {
        keyBytes[i] = (uint8)(i + 1);
    }

    BigNumber K;
    K.SetBinary(keyBytes, 40);

    // Independently compute the expected digest with the SAME field order.
    uint8 t[4] = { 0, 0, 0, 0 };
    Sha1Hash sha;
    sha.UpdateData(account);
    sha.UpdateData(t, 4);
    sha.UpdateData((uint8*)&clientSeed, 4);
    sha.UpdateData((uint8*)&serverSeed, 4);
    sha.UpdateBigNumbers(&K, nullptr);
    sha.Finalize();

    uint8 expected[SHA_DIGEST_LENGTH];
    memcpy(expected, sha.GetDigest(), SHA_DIGEST_LENGTH);

    // Test 1: the correct digest must validate.
    if (GatewayAuth::ValidateDigest(account, clientSeed, serverSeed, K, expected))
    {
        printf("[PASS] correct digest accepted\n");
    }
    else
    {
        printf("[FAIL] correct digest rejected\n");
        ++failures;
    }

    // Test 2: a flipped byte must NOT validate.
    uint8 tampered[SHA_DIGEST_LENGTH];
    memcpy(tampered, expected, SHA_DIGEST_LENGTH);
    tampered[7] ^= 0x01;

    if (!GatewayAuth::ValidateDigest(account, clientSeed, serverSeed, K, tampered))
    {
        printf("[PASS] tampered digest rejected\n");
    }
    else
    {
        printf("[FAIL] tampered digest accepted\n");
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
