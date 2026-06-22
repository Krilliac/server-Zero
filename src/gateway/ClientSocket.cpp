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

#include "Common.h"
#include "Log.h"
#include "Util.h"
#include "ByteBuffer.h"

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

ClientSocket::ClientSocket(void)
    : ClientHandler(),
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
    m_OutBufferSize(65536)
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
        // Post-auth: subsequent packets arrive with their headers decrypted
        // (handle_input_header runs DecryptRecv now that m_Crypt is keyed).
        // Routing of game opcodes to world nodes lands in a later task.
        sLog.outString("ClientSocket: recv from %s (authed acct %u) opcode 0x%04X (%u bytes payload)",
            m_Address.c_str(), m_AccountId, opcode, (uint32)payloadLen);
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

    return 0;
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
