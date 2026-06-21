/*
 * Multi-node Cluster framework — dedicated inter-node network thread (Phase 2).
 *
 * Standalone ACE_Task_Base thread that owns its OWN reactor (off the main world
 * reactor), mirroring RAThread. It accepts inbound peer connections (ClusterLink,
 * defined in the .cpp) and maintains outbound connections to peers for sending.
 *
 * Thread boundaries: this thread never touches Player/Map/game state directly.
 * Inbound frames are parsed here and handed to ClusterMgr's inbound queue, drained
 * by the world thread. Outbound frames are produced by other threads into
 * ClusterMgr's outbound queue and only sent from here.
 */

#ifndef MANGOS_CLUSTERNETWORK_H
#define MANGOS_CLUSTERNETWORK_H

#include <ace/SOCK_Acceptor.h>
#include <ace/SOCK_Stream.h>
#include <ace/Acceptor.h>
#include <ace/Task.h>
#include <ace/INET_Addr.h>

#include "Common.h"

#include <map>

class ClusterLink;
class ACE_Reactor;

typedef ACE_Acceptor<ClusterLink, ACE_SOCK_ACCEPTOR> ClusterAcceptor;

class ClusterThread : public ACE_Task_Base
{
    public:
        explicit ClusterThread(uint16 listenPort, const char* host);
        virtual ~ClusterThread();

        int open(void* unused) override; // bind acceptor + activate the thread
        int svc() override;              // reactor loop + outbound flush
        void Stop();                     // request loop exit (called from world thread)

    private:
        void ensurePeerConnections();    // connect to peers we are missing
        void flushOutbound();            // drain ClusterMgr outbound queue -> peer streams
        void dropPeer(uint32 nodeId);

        ACE_Reactor*     m_reactor;
        ClusterAcceptor* m_acceptor;
        ACE_INET_Addr    m_listenAddr;
        volatile bool    m_running;

        // Outbound (send-only) streams to peers, keyed by nodeId. Owned here.
        std::map<uint32, ACE_SOCK_Stream*> m_peerStreams;
};

#endif // MANGOS_CLUSTERNETWORK_H
