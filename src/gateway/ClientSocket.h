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
 * @file ClientSocket.h
 * @brief Cluster gateway client-facing socket
 *
 * ClientSocket is the gateway's per-connection handler for inbound game
 * clients. It mirrors the structure of the world server's WorldSocket
 * (ACE_Svc_Handler + reactor) but strips out all game logic: in Task 2 it
 * only accepts a connection, emits SMSG_AUTH_CHALLENGE (server seed), and
 * logs the opcode/size of anything the client sends back. The actual auth
 * handshake (decrypt, account lookup, key exchange) lands in Task 3.
 */

#ifndef GATEWAY_H_CLIENTSOCKET
#define GATEWAY_H_CLIENTSOCKET

#include <ace/Basic_Types.h>
#include <ace/Synch_Traits.h>
#include <ace/Svc_Handler.h>
#include <ace/SOCK_Stream.h>
#include <ace/SOCK_Acceptor.h>
#include <ace/Acceptor.h>
#include <ace/Thread_Mutex.h>
#include <ace/Guard_T.h>
#include <ace/Message_Block.h>

#if !defined (ACE_LACKS_PRAGMA_ONCE)
#pragma once
#endif /* ACE_LACKS_PRAGMA_ONCE */

#include "Common.h"
#include "Auth/AuthCrypt.h"
#include "ByteBuffer.h"

#include <string>

class ClientSocket;

typedef ACE_Svc_Handler<ACE_SOCK_STREAM, ACE_NULL_SYNCH>      ClientHandler;
typedef ACE_Acceptor<ClientSocket, ACE_SOCK_ACCEPTOR>         ClientAcceptor;

/**
 * @brief Per-connection handler for an inbound game client.
 *
 * Reference-counted ACE service handler. The reactor owns the object once
 * open() succeeds; it is destroyed when its last reference is dropped.
 */
class ClientSocket : protected ClientHandler
{
    public:
        /// Declare some friends
        friend class ACE_Acceptor<ClientSocket, ACE_SOCK_ACCEPTOR>;
        friend class ClientSocketMgr;

        /// Mutex type used for output synchronization.
        typedef ACE_Thread_Mutex LockType;

        /// Check if socket is closed/closing.
        bool IsClosed(void) const { return closing_; }

        /// Get address of connected peer.
        const std::string& GetRemoteAddress(void) const { return m_Address; }

        /// Build a server header for @p opcode + @p payload and send it.
        /// Before auth the header is plaintext; after auth (Task 3) the
        /// EncryptSend hook below will scramble it.
        /// @return 0 on success, -1 on failure
        int SendPacket(uint16 opcode, const ByteBuffer& payload);

    protected:
        /// Things called by the ACE framework.
        ClientSocket(void);
        virtual ~ClientSocket(void);

        /// Called on open, the void* is the acceptor.
        int open(void*) override;

        /// Called on failures inside of the acceptor; don't call from your code.
        int close(u_long) override;

        /// Called when we can read from the socket.
        int handle_input(ACE_HANDLE = ACE_INVALID_HANDLE) override;

        /// Called when the socket can write.
        int handle_output(ACE_HANDLE = ACE_INVALID_HANDLE) override;

        /// Called when connection is closed or error happens.
        int handle_close(ACE_HANDLE = ACE_INVALID_HANDLE,
            ACE_Reactor_Mask = ACE_Event_Handler::ALL_EVENTS_MASK) override;

    private:
        /// Helper functions for processing incoming data.
        int handle_input_header(void);
        int handle_input_payload(void);
        int handle_input_missing_data(void);

        /// Validate CMSG_AUTH_SESSION, look up the account, verify the digest
        /// and key the AuthCrypt. Returns 0 on success, -1 on failure (the
        /// caller closes the connection on failure).
        int HandleAuthSession(ByteBuffer& recv);

        /// Try to write a framed packet to m_OutBuffer; -1 if no space.
        /// Must be called with m_OutBufferLock held.
        int iSendPacket(uint16 opcode, const ByteBuffer& payload);

    private:
        /// Address of the remote peer.
        std::string m_Address;

        /// Header encryption state. Unused until Task 3, but the EncryptSend /
        /// DecryptRecv hooks are already wired up (no-ops while uninitialized).
        AuthCrypt m_Crypt;

        /// Server seed sent in SMSG_AUTH_CHALLENGE.
        uint32 m_Seed;

        /// Set once the client completes the auth handshake (Task 3).
        bool m_Authed;

        /// Account identity captured at auth time.
        uint32 m_AccountId;
        std::string m_AccountName;
        uint8 m_Security;
        uint8 m_Locale;

        /// Fragment of the received client header (6 bytes when full).
        ACE_Message_Block m_Header;

        /// Storage for the current inbound packet payload.
        ACE_Message_Block m_RecvPct;

        /// Opcode of the packet currently being assembled.
        uint32 m_RecvOpcode;

        /// Mutex protecting output related data.
        LockType m_OutBufferLock;

        /// Buffer used for writing output.
        ACE_Message_Block* m_OutBuffer;

        /// Size of m_OutBuffer.
        size_t m_OutBufferSize;
};

#endif /* GATEWAY_H_CLIENTSOCKET */
