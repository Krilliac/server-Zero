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
 * @file ClientSocket.cpp
 * @brief Cluster gateway client-facing socket implementation
 *
 * This is a deliberately slimmed-down clone of WorldSocket. It keeps the same
 * ACE structure (header+payload reassembly, output buffer, reactor wakeups)
 * but does no game work: on connect it sends a plaintext SMSG_AUTH_CHALLENGE
 * carrying a random server seed, and for every subsequent client packet it
 * just logs the opcode and size. Decryption and the auth handshake arrive in
 * Task 3, where m_Crypt gets keyed and the EncryptSend/DecryptRecv hooks below
 * start doing real work.
 */

#include <ace/Message_Block.h>
#include <ace/OS_NS_string.h>
#include <ace/os_include/netinet/os_tcp.h>
#include <ace/os_include/sys/os_socket.h>
#include <ace/Reactor.h>
#include <ace/INET_Addr.h>

#include "ClientSocket.h"
#include "ClientSocketMgr.h"
#include "GatewayAuth.h"
#include "NodeLink.h"
#include "NodeRegistry.h"
#include "Cluster/GatewayProtocol.h"

#include "Common.h"
#include "Log.h"
#include "Util.h"
#include "ByteBuffer.h"
#include "Config/Config.h"

#include "Database/DatabaseEnv.h"
#include "Auth/BigNumber.h"
#include "Auth/Sha1.h"   // for SHA_DIGEST_LENGTH

/// Login database accessor (defined in Main.cpp).
extern DatabaseType LoginDatabase;

/// Opcode constants (mirror Opcodes.h; defined locally so the gateway does
/// not have to pull in the game's opcode/session headers).
#ifndef GATEWAY_SMSG_AUTH_CHALLENGE
#define GATEWAY_SMSG_AUTH_CHALLENGE 0x1EC
#endif
#ifndef GATEWAY_CMSG_AUTH_SESSION
#define GATEWAY_CMSG_AUTH_SESSION 0x1ED
#endif
#ifndef GATEWAY_SMSG_AUTH_RESPONSE
#define GATEWAY_SMSG_AUTH_RESPONSE 0x1EE
#endif
/// AUTH_OK response code (mirrors AuthResponseCodes::AUTH_OK).
#ifndef GATEWAY_AUTH_OK
#define GATEWAY_AUTH_OK 0x0C
#endif
/// CMSG_PLAYER_LOGIN: the client's "enter world" request. Payload is a single
/// uint64 player guid (little-endian). The gateway intercepts this to route the
/// session to the character's owning node before forwarding.
#ifndef GATEWAY_CMSG_PLAYER_LOGIN
#define GATEWAY_CMSG_PLAYER_LOGIN 0x3D
#endif
/// CMSG_PING / SMSG_PONG: connection keep-alive handled at the socket level (in a
/// non-clustered server this is WorldSocket::HandlePing). There is no per-node
/// socket behind a gateway-fronted session, so the gateway answers the ping
/// ITSELF and must NOT forward it to the node (the node has no socket to ping and
/// rejects CMSG_PING as a not-allowed opcode). CMSG_PING payload is
/// uint32 ping(sequence) + uint32 latency; SMSG_PONG echoes the uint32 sequence.
#ifndef GATEWAY_CMSG_PING
#define GATEWAY_CMSG_PING 0x1DC
#endif
#ifndef GATEWAY_SMSG_PONG
#define GATEWAY_SMSG_PONG 0x1DD
#endif

/// Anti-cheat violation-type numbers the gateway may report (Phase 1 self-test).
/// These are plain numbers that MUST match AntiCheatDefines.h's
/// AntiCheatViolationType on the node — the gateway stays game-independent and
/// does NOT include that enum. (16 = AC_VIOLATION_RATE.)
#ifndef GATEWAY_AC_VIOLATION_RATE
#define GATEWAY_AC_VIOLATION_RATE 16
#endif

#if defined( __GNUC__ )
#pragma pack(1)
#else
#pragma pack(push,1)
#endif

/// Header for packets sent from server to client (size BE + 2-byte opcode).
struct ServerPktHeader
{
    uint16 size;
    uint16 cmd;
};

/// Header for packets sent from client to server (size BE + 4-byte opcode).
struct ClientPktHeader
{
    uint16 size;
    uint32 cmd;
};

#if defined( __GNUC__ )
#pragma pack()
#else
#pragma pack(pop)
#endif

/// Process-wide source of unique client ids (starts at 1; 0 means "unset").
std::atomic<uint32> ClientSocket::s_ClientIdCounter(0);

ClientSocket::ClientSocket(void)
    : ClientHandler(),
    m_ClientId(++s_ClientIdCounter),
    m_SessionOpened(false),
    m_CurrentNodeId(0),
    m_Address(),
    m_Crypt(),
    m_Seed(rand32()),
    m_Authed(false),
    m_AccountId(0),
    m_AccountName(),
    m_Security(0),
    m_Locale(0),
    m_Header(sizeof(ClientPktHeader)),
    m_RecvPct(),
    m_RecvOpcode(0),
    m_OutBufferLock(),
    m_OutBuffer(0),
    m_OutBufferSize(65536),
    m_MigrateState(MIG_NONE),
    m_MigrateDestNode(0),
    m_MigrateOldNode(0),
    m_MigrateCharGuid(0),
    m_MigrateBuffer(),
    m_MigrateLock()
{
    reference_counting_policy().value(ACE_Event_Handler::Reference_Counting_Policy::ENABLED);
}

ClientSocket::~ClientSocket(void)
{
    if (m_OutBuffer)
    {
        m_OutBuffer->release();
    }

    closing_ = true;

    peer().close();
}

/**
 * @brief Opens the handler: stores peer address, registers for input and
 *        sends the plaintext SMSG_AUTH_CHALLENGE.
 *
 * @param a The ACE acceptor hook parameter (unused).
 * @return int Zero on success; otherwise -1.
 */
int ClientSocket::open(void* a)
{
    ACE_UNUSED_ARG(a);

    // Prevent double call to this func.
    if (m_OutBuffer)
    {
        return -1;
    }

    // Hook for the manager (socket options + reactor binding).
    if (sClientSocketMgr->OnSocketOpen(this) == -1)
    {
        return -1;
    }

    // Allocate the output buffer.
    ACE_NEW_RETURN(m_OutBuffer, ACE_Message_Block(m_OutBufferSize), -1);

    // Store peer address.
    ACE_INET_Addr remote_addr;

    if (peer().get_remote_addr(remote_addr) == -1)
    {
        sLog.outError("ClientSocket::open: peer().get_remote_addr errno = %s", ACE_OS::strerror(errno));
        return -1;
    }

    m_Address = remote_addr.get_host_addr();

    // Register with the ACE reactor for read events.
    if (reactor()->register_handler(this, ACE_Event_Handler::READ_MASK) == -1)
    {
        sLog.outError("ClientSocket::open: unable to register client handler errno = %s", ACE_OS::strerror(errno));
        return -1;
    }

    // Reactor takes care of the socket from now on.
    remove_reference();

    sLog.outString("ClientSocket: accepted client %s, sending SMSG_AUTH_CHALLENGE (seed 0x%08X)",
        m_Address.c_str(), m_Seed);

    // Send the startup challenge: a single uint32 server seed.
    ByteBuffer payload;
    payload << m_Seed;

    return SendPacket(GATEWAY_SMSG_AUTH_CHALLENGE, payload);
}

/**
 * @brief Closes the socket and releases its final ACE reference.
 *
 * @return int Always returns zero.
 */
int ClientSocket::close(u_long)
{
    shutdown();

    closing_ = true;

    remove_reference();

    return 0;
}

/**
 * @brief Processes readable socket input.
 *
 * @return int Zero on success; otherwise -1.
 */
int ClientSocket::handle_input(ACE_HANDLE)
{
    if (closing_)
    {
        return -1;
    }

    switch (handle_input_missing_data())
    {
        case -1 :
        {
            if ((errno == EWOULDBLOCK) || (errno == EAGAIN))
            {
                return 0;
            }
        }
        case 0:
        {
            errno = ECONNRESET;
            return -1;
        }
        default:
            return 0;
    }

    ACE_NOTREACHED(return -1);
}

/**
 * @brief Flushes pending outbound socket data.
 *
 * @return int Zero on success; otherwise -1.
 */
int ClientSocket::handle_output(ACE_HANDLE)
{
    ACE_GUARD_RETURN(LockType, Guard, m_OutBufferLock, -1);

    if (closing_)
    {
        return -1;
    }

    const size_t send_len = m_OutBuffer->length();

    if (send_len == 0)
    {
        reactor()->cancel_wakeup(this, ACE_Event_Handler::WRITE_MASK);
        return 0;
    }

#ifdef MSG_NOSIGNAL
    ssize_t n = peer().send(m_OutBuffer->rd_ptr(), send_len, MSG_NOSIGNAL);
#else
    ssize_t n = peer().send(m_OutBuffer->rd_ptr(), send_len);
#endif // MSG_NOSIGNAL

    if (n == 0)
    {
        return -1;
    }
    else if (n == -1)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
        {
            return 0;
        }
        return -1;
    }
    else if (n < (ssize_t)send_len) // partial write
    {
        m_OutBuffer->rd_ptr(static_cast<size_t>(n));
        m_OutBuffer->crunch();
        return 0;
    }
    else // n == send_len
    {
        m_OutBuffer->reset();
        reactor()->cancel_wakeup(this, ACE_Event_Handler::WRITE_MASK);
        return 0;
    }

    ACE_NOTREACHED(return 0);
}

/**
 * @brief Handles socket closure and unregisters from the reactor.
 *
 * @param h The closing handle.
 * @return int Always returns zero.
 */
int ClientSocket::handle_close(ACE_HANDLE h, ACE_Reactor_Mask)
{
    // Tear down the backend session and stop routing inbound node packets here.
    if (m_SessionOpened)
    {
        m_SessionOpened = false;

        // Phase 2 / Task 2: release the session on the node that currently
        // fronts this client (m_CurrentNodeId), not necessarily the pre-world
        // node — the client may have been re-homed at CMSG_PLAYER_LOGIN.
        ByteBuffer releaseMsg;
        releaseMsg << uint32(m_ClientId);
        if (NodeLink* link = sNodeRegistry().Get(m_CurrentNodeId))
        {
            link->SendFrame(GW_SESSION_RELEASE, releaseMsg); // best-effort
        }
        else
        {
            DEBUG_LOG("ClientSocket: GW_SESSION_RELEASE for client %u not sent: node %u link unavailable",
                m_ClientId, m_CurrentNodeId);
        }

        sNodeRegistry().UnregisterClient(m_ClientId);
    }

    {
        ACE_GUARD_RETURN(LockType, Guard, m_OutBufferLock, -1);

        closing_ = true;

        if (h == ACE_INVALID_HANDLE)
        {
            peer().close_writer();
        }
    }

    reactor()->remove_handler(this, ACE_Event_Handler::DONT_CALL | ACE_Event_Handler::ALL_EVENTS_MASK);
    return 0;
}

/**
 * @brief Deliver a server packet received from the backend node to the client.
 *
 * Builds the (encrypted, once keyed) server header and writes the body via the
 * existing SendPacket path. Invoked from the NodeLink thread; SendPacket takes
 * m_OutBufferLock so this is safe against the reactor threads.
 *
 * @param opcode  Server opcode supplied by the node.
 * @param payload Packet body supplied by the node.
 * @return int Zero on success; -1 on failure.
 */
int ClientSocket::DeliverServerPacket(uint16 opcode, const ByteBuffer& payload)
{
    if (closing_)
    {
        return -1;
    }
    return SendPacket(opcode, payload);
}

/**
 * @brief Parses an incoming client packet header.
 *
 * In Task 2 the header is read in the clear (DecryptRecv is a no-op while the
 * crypt is uninitialized). Records the opcode + payload size and sizes the
 * payload buffer accordingly.
 *
 * @return int Zero on success; otherwise -1.
 */
int ClientSocket::handle_input_header(void)
{
    MANGOS_ASSERT(m_Header.length() == sizeof(ClientPktHeader));

    // Auth hook: once keyed (Task 3) this decrypts the 6-byte client header.
    m_Crypt.DecryptRecv((uint8*) m_Header.rd_ptr(), sizeof(ClientPktHeader));

    ClientPktHeader& header = *((ClientPktHeader*) m_Header.rd_ptr());

    EndianConvertReverse(header.size);
    EndianConvert(header.cmd);

    if ((header.size < 4) || (header.size > 10240) || (header.cmd > 10240))
    {
        sLog.outError("ClientSocket::handle_input_header: client %s sent malformed packet size = %d, cmd = %d",
            m_Address.c_str(), header.size, header.cmd);

        errno = EINVAL;
        return -1;
    }

    // header.size counts the 4-byte opcode field; payload is the remainder.
    const uint16 payloadSize = header.size - 4;
    m_RecvOpcode = header.cmd;

    // (Re)size the payload block. After size(), length()==0 and space()==n,
    // so handle_input_missing_data() reads exactly payloadSize bytes (or skips
    // straight to handle_input_payload() when there is no payload).
    if (m_RecvPct.size(payloadSize) == -1)
    {
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

/**
 * @brief Handles a fully received client packet.
 *
 * Task 2 only logs it; routing/decryption come later.
 *
 * @return int Zero on success; otherwise -1.
 */
int ClientSocket::handle_input_payload(void)
{
    const size_t payloadLen = m_RecvPct.length();
    const uint32 opcode = m_RecvOpcode;

    // Wrap the assembled payload in a ByteBuffer for structured parsing.
    ByteBuffer recv;
    if (payloadLen)
    {
        recv.append((const uint8*)m_RecvPct.rd_ptr(), payloadLen);
    }

    // Reset receive state up front so every return path leaves us ready for
    // the next packet.
    m_RecvPct.reset();
    m_Header.reset();
    m_RecvOpcode = 0;

    int rc = 0;

    if (!m_Authed)
    {
        // Pre-auth: only CMSG_AUTH_SESSION is accepted. Anything else is a
        // protocol violation and closes the connection.
        if (opcode == GATEWAY_CMSG_AUTH_SESSION)
        {
            if (HandleAuthSession(recv) == -1)
            {
                errno = ECONNRESET;
                rc = -1;
            }
        }
        else
        {
            sLog.outError("ClientSocket: client %s sent opcode 0x%04X before auth; closing",
                m_Address.c_str(), opcode);
            errno = ECONNRESET;
            rc = -1;
        }
    }
    else
    {
        // Intercept CMSG_PING: a connection keep-alive (the client sends one every
        // ~30s). In a non-clustered server this is answered at the socket level by
        // WorldSocket::HandlePing; behind the gateway there is no per-node socket,
        // so we answer it HERE and never forward it to the node (which has no socket
        // to ping and would reject CMSG_PING as a not-allowed opcode). Payload is
        // uint32 ping(sequence) + uint32 latency; reply SMSG_PONG echoes the
        // sequence. Done before HandlePlayerLogin / migration buffering / the
        // GW_CLIENT_PACKET forward so the ping is fully handled at the gateway.
        if (opcode == GATEWAY_CMSG_PING)
        {
            uint32 pingSeq = 0;
            try
            {
                recv >> pingSeq; // latency follows but is not needed for the pong
            }
            catch (ByteBufferException&)
            {
                sLog.outError("ClientSocket: client %u sent a malformed CMSG_PING; ignoring",
                    m_ClientId);
                return rc;
            }

            ByteBuffer pong;
            pong << uint32(pingSeq);
            SendPacket(GATEWAY_SMSG_PONG, pong); // encrypts via iSendPacket
            return rc; // do NOT forward CMSG_PING to the node
        }

        // Phase 2 / Task 3: intercept CMSG_PLAYER_LOGIN (enter-world). Resolve
        // the character's owning node and, if it differs from the node currently
        // fronting this player-less session, re-home: release on the old node and
        // open on the target. This is cheap (no player loaded yet). The login
        // packet itself is then forwarded to the now-current node below.
        if (opcode == GATEWAY_CMSG_PLAYER_LOGIN)
        {
            HandlePlayerLogin(recv);
        }

        // Post-auth: subsequent packets arrive with their headers decrypted
        // (handle_input_header runs DecryptRecv now that m_Crypt is keyed).
        //
        // Migration window (§5 step 3): while this client is MIG_MIGRATING the
        // decrypted client->server packet is APPENDED to m_MigrateBuffer instead
        // of being forwarded. This is the buffer/replay window — it stops node A
        // from double-processing the frozen player and prevents loss; the buffer
        // is replayed to node B (or flushed back to A on abort) on GW_SESSION_READY.
        // Otherwise the packet is tunnelled to the backend node as a
        // GW_CLIENT_PACKET (clientId, opcode, raw payload).
        {
            ACE_GUARD_RETURN(LockType, MigGuard, m_MigrateLock, rc);

            if (m_MigrateState == MIG_MIGRATING)
            {
                BufferedPacket bp;
                bp.opcode = (uint16)opcode;
                if (payloadLen)
                {
                    bp.payload.assign(recv.contents(), recv.contents() + payloadLen);
                }
                m_MigrateBuffer.push_back(bp);
                DEBUG_LOG("ClientSocket: buffered opcode 0x%04X from client %u during migration (%u queued)",
                    opcode, m_ClientId, (uint32)m_MigrateBuffer.size());
                return rc;
            }
        }

        // Normal forward path. If the node link is down the forward fails
        // best-effort; we just log it and keep going.
        ByteBuffer fwd;
        fwd << uint32(m_ClientId);
        fwd << uint16(opcode);
        if (payloadLen)
        {
            fwd.append(recv.contents(), payloadLen);
        }

        // Route to the node that currently fronts this client (m_CurrentNodeId),
        // which CMSG_PLAYER_LOGIN above may have just re-homed.
        NodeLink* link = sNodeRegistry().Get(m_CurrentNodeId);
        if (!link || !link->SendFrame(GW_CLIENT_PACKET, fwd))
        {
            DEBUG_LOG("ClientSocket: drop opcode 0x%04X from client %u (acct %u): node %u link down",
                opcode, m_ClientId, m_AccountId, m_CurrentNodeId);
        }
    }

    return rc;
}

/**
 * @brief Validate CMSG_AUTH_SESSION and initialize the AuthCrypt.
 *
 * Parses the packet in the same field order as WorldSocket::HandleAuthSession
 * (build:uint32, serverId/unk:uint32, account:cstring, clientSeed:uint32,
 * digest:20 bytes), looks the account up in the login DB, recomputes the
 * challenge digest via GatewayAuth::ValidateDigest and, on success, keys the
 * crypt so all later traffic is encrypted.
 *
 * @param recv The CMSG_AUTH_SESSION payload (opcode already stripped).
 * @return int Zero on success; -1 on any failure (caller closes the socket).
 */
int ClientSocket::HandleAuthSession(ByteBuffer& recv)
{
    uint32 build = 0;
    uint32 unk2 = 0;
    std::string account;
    uint32 clientSeed = 0;
    uint8 digest[SHA_DIGEST_LENGTH];

    // Same field order as WorldSocket::HandleAuthSession.
    try
    {
        recv >> build;
        recv >> unk2;
        recv >> account;
        recv >> clientSeed;
        recv.read(digest, SHA_DIGEST_LENGTH);
    }
    catch (ByteBufferException&)
    {
        sLog.outError("ClientSocket::HandleAuthSession: client %s sent a malformed CMSG_AUTH_SESSION",
            m_Address.c_str());
        return -1;
    }

    if (account.empty())
    {
        sLog.outError("ClientSocket::HandleAuthSession: client %s sent an empty account name",
            m_Address.c_str());
        return -1;
    }

    // Escape the account name for the lookup. The hash below must use the raw
    // packet bytes (exactly as WorldSocket does), so keep a separate copy.
    std::string safeAccount = account;
    LoginDatabase.escape_string(safeAccount);

    QueryResult* result = LoginDatabase.PQuery(
        "SELECT `id`, `gmlevel`, `sessionkey`, `locale` FROM `account` WHERE `username` = '%s'",
        safeAccount.c_str());

    if (!result)
    {
        sLog.outError("ClientSocket::HandleAuthSession: unknown account '%s' from %s",
            account.c_str(), m_Address.c_str());
        return -1;
    }

    Field* fields = result->Fetch();

    uint32 id       = fields[0].GetUInt32();
    uint8  security = (uint8)fields[1].GetUInt16();
    std::string sessionkey = fields[2].GetCppString();
    uint8  locale   = fields[3].GetUInt8();

    delete result;

    BigNumber K;
    K.SetHexStr(sessionkey.c_str());

    if (!GatewayAuth::ValidateDigest(account, clientSeed, m_Seed, K, digest))
    {
        sLog.outError("ClientSocket::HandleAuthSession: digest mismatch for account '%s' from %s",
            account.c_str(), m_Address.c_str());
        return -1;
    }

    // Key the crypt: from here on header bytes are scrambled in both directions.
    m_Crypt.SetKey(K.AsByteArray(40), 40);
    m_Crypt.Init();

    m_Authed      = true;
    m_AccountId   = id;
    m_AccountName = account;
    m_Security    = security;
    m_Locale      = locale;

    sLog.outString("gateway: client %s authed (acct %u)", account.c_str(), id);

    // Signal success on the wire. m_Crypt is now keyed, so iSendPacket's
    // EncryptSend scrambles this response's 4-byte server header. The body is
    // a single AUTH_OK byte, matching SMSG_AUTH_RESPONSE's minimal classic form.
    ByteBuffer response;
    response << uint8(GATEWAY_AUTH_OK);
    if (SendPacket(GATEWAY_SMSG_AUTH_RESPONSE, response) == -1)
    {
        sLog.outError("ClientSocket::HandleAuthSession: failed to send SMSG_AUTH_RESPONSE to %s",
            m_Address.c_str());
        return -1;
    }

    // Register with the registry so inbound node packets (from any link) route
    // back here, then tell the pre-world node to open a backend session for this
    // client. The send is best-effort: if the node is down it fails gracefully
    // (logged) and the gateway keeps serving the already-authed client.
    //
    // Phase 2 / Task 2: the pre-world node (lowest connected id) fronts the
    // session through char-enum. m_CurrentNodeId records which node fronts this
    // client; CMSG_PLAYER_LOGIN may re-home it to the character's owning node.
    sNodeRegistry().RegisterClient(m_ClientId, this);
    m_SessionOpened = true;

    NodeLink* preWorld = sNodeRegistry().PreWorldNode();
    m_CurrentNodeId = preWorld ? preWorld->NodeId() : 1;

    NodeLink* link = sNodeRegistry().Get(m_CurrentNodeId);
    if (!link || !link->SendFrame(GW_SESSION_OPEN, BuildSessionOpen()))
    {
        sLog.outError("ClientSocket: GW_SESSION_OPEN for client %u (acct %u) not delivered to node %u (node link down)",
            m_ClientId, m_AccountId, m_CurrentNodeId);
    }

    // Anti-cheat channel self-test (Phase 1, diagnostic; off by default). When
    // Gateway.SelfTestAcEvent is set, send ONE benign GW_AC_EVENT per client right
    // after session open, so a full gateway -> node -> AntiCheatMgr round-trip can
    // be observed in the node log. Pure diagnostic — not a real detection.
    if (sConfig.GetBoolDefault("Gateway.SelfTestAcEvent", false))
    {
        ReportAcViolation(GATEWAY_AC_VIOLATION_RATE, 1, "selftest");
        sLog.outString("ClientSocket: self-test GW_AC_EVENT sent for client %u (acct %u) to node %u",
            m_ClientId, m_AccountId, m_CurrentNodeId);
    }

    return 0;
}

/**
 * @brief Build the GW_SESSION_OPEN payload for this client.
 *
 * Layout mirrors GatewayProtocol.h: uint32 clientId, uint32 accountId,
 * uint32 security, uint8 locale, string accountName. Reused both at auth
 * (initial pre-world open) and at CMSG_PLAYER_LOGIN re-home (open on the
 * character's owning node).
 */
ByteBuffer ClientSocket::BuildSessionOpen() const
{
    ByteBuffer openMsg;
    openMsg << uint32(m_ClientId);
    openMsg << uint32(m_AccountId);
    openMsg << uint32(m_Security);
    openMsg << uint8(m_Locale);
    openMsg << m_AccountName;
    return openMsg;
}

/**
 * @brief Report a gateway-detected anti-cheat violation to this client's node.
 *
 * Builds the GW_AC_EVENT payload (uint32 clientId, uint8 type, uint8 severity,
 * string detail) and sends it on the node link that currently fronts this client
 * (m_CurrentNodeId), mirroring the BuildSessionOpen/GW_SESSION_OPEN send path.
 * Best-effort: if the link is down the event is dropped (DEBUG_LOG), never queued.
 */
void ClientSocket::ReportAcViolation(uint8 type, uint8 severity, const char* detail)
{
    ByteBuffer payload;
    payload << uint32(m_ClientId);
    payload << uint8(type);
    payload << uint8(severity);
    payload << std::string(detail ? detail : "");

    NodeLink* link = sNodeRegistry().Get(m_CurrentNodeId);
    if (!link || !link->SendFrame(GW_AC_EVENT, payload))
    {
        DEBUG_LOG("ClientSocket: GW_AC_EVENT for client %u (type %u, severity %u) not delivered to node %u (link down)",
            m_ClientId, (uint32)type, (uint32)severity, m_CurrentNodeId);
    }
}

/**
 * @brief Resolve a CMSG_PLAYER_LOGIN target node and re-home if needed.
 *
 * Reads the low 32 bits of the 8-byte player guid from the (un-consumed) login
 * payload, asks the registry which node owns that character, and — if it is a
 * different, reachable node than the one currently fronting this session —
 * releases the player-less session on the old node and opens it on the target,
 * updating m_CurrentNodeId. The caller forwards the login packet afterwards, so
 * this does NOT touch @p recv's read position.
 *
 * @param recv The CMSG_PLAYER_LOGIN payload (read pointer at the start).
 */
void ClientSocket::HandlePlayerLogin(const ByteBuffer& recv)
{
    // Payload is a single little-endian uint64 guid; we only need the low dword.
    if (recv.size() < 8)
    {
        sLog.outError("ClientSocket: client %u sent a short CMSG_PLAYER_LOGIN (%u bytes); ignoring affinity routing",
            m_ClientId, (uint32)recv.size());
        return;
    }

    const uint8* p = recv.contents();
    uint32 guidLow = (uint32)p[0]
                   | ((uint32)p[1] << 8)
                   | ((uint32)p[2] << 16)
                   | ((uint32)p[3] << 24);

    uint32 target = sNodeRegistry().NodeForCharacter(guidLow);

    if (target == 0 || !sNodeRegistry().Get(target))
    {
        sLog.outString("gateway: char %u: no/unknown affinity, staying on node %u",
            guidLow, m_CurrentNodeId);
        return;
    }

    if (target == m_CurrentNodeId)
    {
        return; // already on the owning node; nothing to do
    }

    sLog.outString("gateway: routing char %u to node %u (re-home from %u)",
        guidLow, target, m_CurrentNodeId);

    // Release the player-less session on the old node (best-effort).
    if (NodeLink* oldLink = sNodeRegistry().Get(m_CurrentNodeId))
    {
        ByteBuffer releaseMsg;
        releaseMsg << uint32(m_ClientId);
        oldLink->SendFrame(GW_SESSION_RELEASE, releaseMsg);
    }

    // Open the session on the target node, carrying the same account identity
    // captured at auth (GW_SESSION_OPEN payload is rebuilt from this client).
    NodeLink* newLink = sNodeRegistry().Get(target);
    if (!newLink || !newLink->SendFrame(GW_SESSION_OPEN, BuildSessionOpen()))
    {
        sLog.outError("ClientSocket: re-home GW_SESSION_OPEN for client %u (char %u) to node %u failed (link down)",
            m_ClientId, guidLow, target);
    }

    m_CurrentNodeId = target;
}

/**
 * @brief Begin a migration for this client (GW_MIGRATE_REQUEST from node A).
 *
 * §5 steps 2-4. Arrives on the NodeLink thread (the SOURCE node asked the
 * gateway to flip this client to @p destNode). Marks the client MIG_MIGRATING
 * so the reactor's forward path starts buffering, records the dest/old node and
 * char guid, then sends GW_SESSION_PREPARE to the destination node so it stages
 * (loads) the migrating character from the shared DB.
 *
 * If the destination node link is unavailable we abort immediately: tell node A
 * to un-quiesce (GW_MIGRATE_ABORT), flush whatever was buffered back to it, and
 * return to MIG_NONE — the player stays put.
 *
 * @param destNode The node to migrate this client to.
 * @param charGuid The low 32 bits of the migrating player's guid.
 */
void ClientSocket::OnMigrateRequest(uint32 destNode, uint32 charGuid)
{
    NodeLink* destLink = sNodeRegistry().Get(destNode);

    {
        ACE_GUARD(LockType, MigGuard, m_MigrateLock);

        if (m_MigrateState == MIG_MIGRATING)
        {
            sLog.outError("ClientSocket: GW_MIGRATE_REQUEST for client %u already migrating (%u->%u); ignoring new request to node %u",
                m_ClientId, m_MigrateOldNode, m_MigrateDestNode, destNode);
            return;
        }

        // No reachable destination: refuse before we start buffering.
        if (!destLink)
        {
            sLog.outError("ClientSocket: GW_MIGRATE_REQUEST for client %u to node %u rejected (dest link unavailable); aborting, player stays on node %u",
                m_ClientId, destNode, m_CurrentNodeId);

            // Tell the source (current) node to un-quiesce. Nothing was buffered
            // yet, so there is nothing to flush.
            if (NodeLink* oldLink = sNodeRegistry().Get(m_CurrentNodeId))
            {
                ByteBuffer abortMsg;
                abortMsg << uint32(m_ClientId);
                oldLink->SendFrame(GW_MIGRATE_ABORT, abortMsg);
            }
            return;
        }

        // Enter the buffering window. From here, the reactor thread appends
        // client->server packets to m_MigrateBuffer instead of forwarding.
        m_MigrateState   = MIG_MIGRATING;
        m_MigrateDestNode = destNode;
        m_MigrateOldNode  = m_CurrentNodeId;
        m_MigrateCharGuid = charGuid;
        m_MigrateBuffer.clear();
    }

    // GW_SESSION_PREPARE (gateway -> node B): uint32 clientId, uint32 accountId,
    // uint32 charGuid, uint8 locale, uint32 security, uint32 destNode.
    ByteBuffer prep;
    prep << uint32(m_ClientId);
    prep << uint32(m_AccountId);
    prep << uint32(charGuid);
    prep << uint8(m_Locale);
    prep << uint32(m_Security);
    prep << uint32(destNode);

    if (!destLink->SendFrame(GW_SESSION_PREPARE, prep))
    {
        // The link dropped between the Get() above and now: treat as abort.
        sLog.outError("ClientSocket: GW_SESSION_PREPARE for client %u to node %u failed (link down); aborting",
            m_ClientId, destNode);
        OnSessionReady(false);
        return;
    }

    sLog.outString("gateway: client %u migrating %u->%u, sent GW_SESSION_PREPARE (char %u)",
        m_ClientId, m_MigrateOldNode, destNode, charGuid);
}

/**
 * @brief Complete (or abort) a migration (GW_SESSION_READY from node B).
 *
 * §5 steps 5-6. Arrives on the NodeLink thread (the DEST node reports whether
 * it staged the character).
 *
 * On ok: atomically (under m_MigrateLock) switch the forward-target to the dest
 * node, release the old node, replay the buffered client packets to the new
 * node, and return to MIG_NONE.
 *
 * On !ok: tell the old node to un-quiesce (GW_MIGRATE_ABORT), flush the buffered
 * packets back to the old node (the player stays there), and return to MIG_NONE.
 *
 * Server->client packets are NOT involved here: they keep flowing from EITHER
 * node straight through DeliverServerPacket; only the client->server target moves.
 *
 * @param ok True if node B staged the session; false to abort and stay put.
 */
void ClientSocket::OnSessionReady(bool ok)
{
    ACE_GUARD(LockType, MigGuard, m_MigrateLock);

    if (m_MigrateState != MIG_MIGRATING)
    {
        sLog.outError("ClientSocket: GW_SESSION_READY(ok=%u) for client %u while not migrating; ignoring",
            ok ? 1 : 0, m_ClientId);
        return;
    }

    const uint32 oldNode = m_MigrateOldNode;
    const uint32 destNode = m_MigrateDestNode;

    if (ok)
    {
        // Atomic switch: the forward-target becomes node B. m_CurrentNodeId is
        // the single source of truth for routing (used by the reactor thread and
        // handle_close); we hold m_MigrateLock around the switch + replay so no
        // client packet can slip onto the wrong node.
        m_CurrentNodeId = destNode;

        // Release the (now inert, already-saved) session on node A.
        if (NodeLink* oldLink = sNodeRegistry().Get(oldNode))
        {
            ByteBuffer releaseMsg;
            releaseMsg << uint32(m_ClientId);
            oldLink->SendFrame(GW_SESSION_RELEASE, releaseMsg);
        }

        // Replay the buffered client->server packets, in FIFO order, to node B.
        NodeLink* destLink = sNodeRegistry().Get(destNode);
        const uint32 replayed = (uint32)m_MigrateBuffer.size();
        while (!m_MigrateBuffer.empty())
        {
            BufferedPacket& bp = m_MigrateBuffer.front();
            ByteBuffer fwd;
            fwd << uint32(m_ClientId);
            fwd << uint16(bp.opcode);
            if (!bp.payload.empty())
            {
                fwd.append(&bp.payload[0], bp.payload.size());
            }
            if (!destLink || !destLink->SendFrame(GW_CLIENT_PACKET, fwd))
            {
                DEBUG_LOG("ClientSocket: replay drop opcode 0x%04X from client %u: node %u link down",
                    bp.opcode, m_ClientId, destNode);
            }
            m_MigrateBuffer.pop_front();
        }

        m_MigrateState = MIG_NONE;

        sLog.outString("gateway: client %u migrated %u->%u, replayed %u buffered packet(s)",
            m_ClientId, oldNode, destNode, replayed);
    }
    else
    {
        // Abort: node B failed to stage. Tell node A to un-quiesce, then flush the
        // buffered packets back to A (forward, don't drop — the player stays there).
        NodeLink* oldLink = sNodeRegistry().Get(oldNode);

        if (oldLink)
        {
            ByteBuffer abortMsg;
            abortMsg << uint32(m_ClientId);
            oldLink->SendFrame(GW_MIGRATE_ABORT, abortMsg);
        }

        const uint32 flushed = (uint32)m_MigrateBuffer.size();
        while (!m_MigrateBuffer.empty())
        {
            BufferedPacket& bp = m_MigrateBuffer.front();
            ByteBuffer fwd;
            fwd << uint32(m_ClientId);
            fwd << uint16(bp.opcode);
            if (!bp.payload.empty())
            {
                fwd.append(&bp.payload[0], bp.payload.size());
            }
            if (!oldLink || !oldLink->SendFrame(GW_CLIENT_PACKET, fwd))
            {
                DEBUG_LOG("ClientSocket: abort-flush drop opcode 0x%04X from client %u: node %u link down",
                    bp.opcode, m_ClientId, oldNode);
            }
            m_MigrateBuffer.pop_front();
        }

        m_MigrateState = MIG_NONE;

        sLog.outString("gateway: migration aborted, client %u stays on node %u (flushed %u buffered packet(s))",
            m_ClientId, oldNode, flushed);
    }
}

/**
 * @brief Receives and assembles any missing header or payload data.
 *
 * @return int A receive state code, or -1 on error.
 */
int ClientSocket::handle_input_missing_data(void)
{
    char buf[4096];

    ACE_Data_Block db(sizeof(buf),
        ACE_Message_Block::MB_DATA,
        buf,
        0,
        0,
        ACE_Message_Block::DONT_DELETE,
        0);

    ACE_Message_Block message_block(&db,
        ACE_Message_Block::DONT_DELETE,
        0);

    const size_t recv_size = message_block.space();

    const ssize_t n = peer().recv(message_block.wr_ptr(), recv_size);

    if (n <= 0)
    {
        return (int)n;
    }

    message_block.wr_ptr(n);

    while (message_block.length() > 0)
    {
        if (m_Header.space() > 0)
        {
            // Need to receive (more of) the header.
            const size_t to_header = (message_block.length() > m_Header.space() ? m_Header.space() : message_block.length());
            m_Header.copy(message_block.rd_ptr(), to_header);
            message_block.rd_ptr(to_header);

            if (m_Header.space() > 0)
            {
                // Couldn't receive the whole header this time.
                MANGOS_ASSERT(message_block.length() == 0);
                errno = EWOULDBLOCK;
                return -1;
            }

            // Got a fresh header.
            if (handle_input_header() == -1)
            {
                MANGOS_ASSERT((errno != EWOULDBLOCK) && (errno != EAGAIN));
                return -1;
            }
        }

        // Now read whatever payload is still missing.
        if (m_RecvPct.space() > 0)
        {
            const size_t to_data = (message_block.length() > m_RecvPct.space() ? m_RecvPct.space() : message_block.length());
            m_RecvPct.copy(message_block.rd_ptr(), to_data);
            message_block.rd_ptr(to_data);

            if (m_RecvPct.space() > 0)
            {
                // Couldn't receive the whole payload this time.
                MANGOS_ASSERT(message_block.length() == 0);
                errno = EWOULDBLOCK;
                return -1;
            }
        }

        // Full packet received.
        if (handle_input_payload() == -1)
        {
            MANGOS_ASSERT((errno != EWOULDBLOCK) && (errno != EAGAIN));
            return -1;
        }
    }

    return size_t(n) == recv_size ? 1 : 2;
}

/**
 * @brief Sends a framed packet (locks the output buffer).
 *
 * @param opcode  Server opcode.
 * @param payload Packet body.
 * @return int Zero on success; otherwise -1.
 */
int ClientSocket::SendPacket(uint16 opcode, const ByteBuffer& payload)
{
    ACE_GUARD_RETURN(LockType, Guard, m_OutBufferLock, -1);

    if (closing_)
    {
        return -1;
    }

    if (iSendPacket(opcode, payload) == -1)
    {
        // Task 2 has no queue: a full buffer is simply a send failure.
        sLog.outError("ClientSocket::SendPacket: no space in output buffer for peer = %s", m_Address.c_str());
        return -1;
    }

    if (reactor()->schedule_wakeup(this, ACE_Event_Handler::WRITE_MASK) == -1)
    {
        sLog.outError("ClientSocket::SendPacket failed setting WRITE mask, peer = %s", m_Address.c_str());
        return -1;
    }

    return 0;
}

/**
 * @brief Serializes a packet into the outbound buffer (caller holds the lock).
 *
 * @param opcode  Server opcode.
 * @param payload Packet body.
 * @return int Zero on success; otherwise -1.
 */
int ClientSocket::iSendPacket(uint16 opcode, const ByteBuffer& payload)
{
    if (m_OutBuffer->space() < payload.size() + sizeof(ServerPktHeader))
    {
        errno = ENOBUFS;
        return -1;
    }

    ServerPktHeader header;
    header.cmd = opcode;
    header.size = (uint16)payload.size() + 2; // +2 for the opcode field

    EndianConvertReverse(header.size);
    EndianConvert(header.cmd);

    // Auth hook: plaintext until m_Crypt is keyed in Task 3, then encrypted.
    m_Crypt.EncryptSend((uint8*) & header, sizeof(header));

    if (m_OutBuffer->copy((char*) & header, sizeof(header)) == -1)
    {
        ACE_ASSERT(false);
    }

    if (!payload.empty())
    {
        if (m_OutBuffer->copy((char*) payload.contents(), payload.size()) == -1)
        {
            ACE_ASSERT(false);
        }
    }

    return 0;
}
