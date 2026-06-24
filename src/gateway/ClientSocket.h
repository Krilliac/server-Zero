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

#include "EdgeChecks.h"          // RateLimiter + ProtocolValidator (Phase 2)
#include "SpeedHackDetector.h"   // SpeedHackDetector (Phase 3)

#include <atomic>
#include <deque>
#include <string>
#include <vector>

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

        /// Deliver a server packet that arrived from the backend node over the
        /// NodeLink. Builds the (encrypted) server header and writes it to the
        /// client. Called from the NodeLink thread; safe via SendPacket's lock.
        /// @return 0 on success, -1 on failure
        int DeliverServerPacket(uint16 opcode, const ByteBuffer& payload);

        /// Gateway-assigned, process-unique id used to key this connection in
        /// the NodeLink's clientId -> ClientSocket map.
        uint32 GetClientId() const { return m_ClientId; }

        /// Migration control frames surfaced from the NodeLink thread (via the
        /// registry's clientId -> ClientSocket lookup). The gateway is the
        /// orchestrator of the §5 migration handshake.
        ///
        /// OnMigrateRequest arrives from the SOURCE node (GW_MIGRATE_REQUEST):
        /// start buffering this client's packets and prepare the dest node.
        /// OnSessionReady arrives from the DEST node (GW_SESSION_READY): commit
        /// the switch + replay, or abort and keep the player on the old node.
        void OnMigrateRequest(uint32 destNode, uint32 charGuid);
        void OnSessionReady(bool ok);

        /// Anti-cheat channel (Phase 1): report a gateway-detected violation for
        /// this client to its owning node. Builds a GW_AC_EVENT payload
        /// (clientId, type, severity, detail) and sends it on this client's node
        /// link (sNodeRegistry().Get(m_CurrentNodeId)->SendFrame), exactly like
        /// BuildSessionOpen/GW_SESSION_OPEN. No-op + DEBUG_LOG if the link is down.
        /// `type`/`severity` are plain numbers: the gateway is game-independent and
        /// does not include the node's AntiCheatViolationType enum.
        void ReportAcViolation(uint8 type, uint8 severity, const char* detail);

        /// Phase 3: load the Gateway.AntiSpeed.* keys once at boot into the
        /// process-wide SpeedHackConfig. Called from Main.cpp before the acceptor
        /// opens. SpeedConfig() returns that config by const ref; every
        /// ClientSocket's m_speedDetector references it.
        static void LoadSpeedConfig();
        static const SpeedHackConfig& SpeedConfig();

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

        /// Serialize the GW_SESSION_OPEN payload (clientId, accountId, security,
        /// locale, accountName) for this client. Used at auth and on re-home.
        ByteBuffer BuildSessionOpen() const;

        /// Intercept CMSG_PLAYER_LOGIN: read the character guid, resolve its
        /// owning node and re-home the (player-less) session there if needed.
        /// Does not consume @p recv (the caller forwards the login afterwards).
        void HandlePlayerLogin(const ByteBuffer& recv);

        /// Phase 3: true if @p opcode carries a bare MovementInfo whose offset-4
        /// uint32 client time the speedhack detector watches.
        static bool IsWatchedMoveOpcode(uint32 opcode);

    private:
        /// Process-wide monotonic source for m_ClientId.
        static std::atomic<uint32> s_ClientIdCounter;

        /// Gateway-assigned unique id for this connection (NodeLink routing key).
        uint32 m_ClientId;

        /// True once GW_SESSION_OPEN has been registered/sent for this client.
        bool m_SessionOpened;

        /// The backend node that currently fronts this (player-less) session.
        /// Set at auth to the pre-world node; re-homed at CMSG_PLAYER_LOGIN to
        /// the character's owning node. All post-auth GW_CLIENT_PACKET /
        /// GW_SESSION_RELEASE frames route to sNodeRegistry().Get(this).
        uint32 m_CurrentNodeId;

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

        // --- Phase 2: gateway edge anti-cheat ------------------------------
        /// Per-connection rate limiter + protocol validator (configured at auth
        /// from the boot-time EdgeCheckConfig). Touched only on the reactor thread
        /// servicing this socket, so no extra lock is needed (ACE_TP_Reactor
        /// dispatches one handler on one thread at a time).
        RateLimiter        m_RateLimiter;
        ProtocolValidator  m_Protocol;

        /// Set true once CMSG_PLAYER_LOGIN has been forwarded for this socket.
        /// Gates the protocol "gameplay opcode before world entry" check.
        bool m_WorldEntered;

        /// True once this connection has been registered with SessionGuard (so
        /// handle_close unregisters exactly once). The SessionGuard key is
        /// m_AccountId + m_Address.
        bool m_SessionGuarded;

        // --- Phase 3: independent-clock speedhack detector ------------------
        /// Per-connection windowed speedhack detector. References the process-wide
        /// SpeedHackConfig loaded at boot. Reactor-thread only, no locks.
        SpeedHackDetector  m_speedDetector;

        // --- Migration orchestration (Phase 3 Task 2) -----------------------
        //
        // The gateway is the migration orchestrator. While a client is
        // MIG_MIGRATING, client->server packets are appended to m_MigrateBuffer
        // instead of being forwarded; on GW_SESSION_READY the forward-target is
        // switched atomically and the buffer is replayed (FIFO) to the new node
        // (or flushed back to the old node on abort).

        /// Per-client migration phase. NONE = normal forward; MIGRATING = the
        /// buffer/prepare/switch window between GW_MIGRATE_REQUEST and
        /// GW_SESSION_READY.
        enum MigrateState { MIG_NONE = 0, MIG_MIGRATING = 1 };

        /// A single buffered client->server packet held during the window.
        struct BufferedPacket
        {
            uint16 opcode;
            std::vector<uint8> payload;
        };

        /// Current migration phase (read by the reactor thread on the forward
        /// path; written by the NodeLink thread). Guarded by m_MigrateLock.
        MigrateState m_MigrateState;

        /// Destination node the gateway is migrating this client TO.
        uint32 m_MigrateDestNode;

        /// Node that fronted this client at migration start (the release/abort
        /// target).
        uint32 m_MigrateOldNode;

        /// Character guid being migrated (forwarded in GW_SESSION_PREPARE).
        uint32 m_MigrateCharGuid;

        /// FIFO of client->server packets captured during the migration window.
        std::deque<BufferedPacket> m_MigrateBuffer;

        /// Guards m_MigrateState + the migration fields + m_MigrateBuffer. Held
        /// briefly on the reactor thread (buffer-or-forward decision) and on the
        /// NodeLink thread (request/ready transitions). Distinct from
        /// m_OutBufferLock so server->client delivery is never blocked by it.
        LockType m_MigrateLock;
};

#endif /* GATEWAY_H_CLIENTSOCKET */
