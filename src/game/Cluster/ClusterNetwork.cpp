/*
 * Multi-node Cluster framework — dedicated inter-node network thread (Phase 2).
 */

#include <ace/Reactor.h>
#include <ace/TP_Reactor.h>
#include <ace/Svc_Handler.h>
#include <ace/SOCK_Connector.h>
#include <ace/Synch_Traits.h>
#include <ace/os_include/os_netdb.h>

#include "ClusterNetwork.h"
#include "ClusterMessage.h"
#include "ClusterMgr.h"
#include "World.h"
#include "Log.h"
#include "ByteBuffer.h"

#include <vector>

// ---------------------------------------------------------------------------
// ClusterLink — inbound peer connection (one per accepted socket). Parses the
// length-prefixed frame stream and hands frames off; never touches game state.
// ---------------------------------------------------------------------------
class ClusterLink : public ACE_Svc_Handler<ACE_SOCK_STREAM, ACE_NULL_SYNCH>
{
        typedef ACE_Svc_Handler<ACE_SOCK_STREAM, ACE_NULL_SYNCH> Base;

    public:
        friend class ACE_Acceptor<ClusterLink, ACE_SOCK_ACCEPTOR>;

        ClusterLink() : Base()
        {
            reference_counting_policy().value(ACE_Event_Handler::Reference_Counting_Policy::ENABLED);
        }

        int open(void* unused) override
        {
            if (reactor()->register_handler(this, ACE_Event_Handler::READ_MASK) == -1)
            {
                sLog.outError("ClusterLink::open: register_handler failed");
                return -1;
            }
            return 0;
        }

        int handle_input(ACE_HANDLE = ACE_INVALID_HANDLE) override
        {
            uint8 tmp[8192];
            ssize_t got = peer().recv(tmp, sizeof(tmp));
            if (got <= 0)
                return -1; // peer closed / error -> reactor closes us

            m_buf.insert(m_buf.end(), tmp, tmp + got);
            parseFrames();
            return 0;
        }

        int handle_close(ACE_HANDLE h = ACE_INVALID_HANDLE,
                         ACE_Reactor_Mask mask = ACE_Event_Handler::ALL_EVENTS_MASK) override
        {
            return Base::handle_close(h, mask);
        }

    private:
        // Consume as many complete [len][type][payload] frames as are buffered.
        void parseFrames()
        {
            size_t off = 0;
            while (m_buf.size() - off >= ClusterFrame::HEADER_SIZE)
            {
                const uint8* p = &m_buf[off];
                uint32 len = (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
                uint8  type = p[4];

                if (len > ClusterFrame::MAX_PAYLOAD)
                {
                    sLog.outError("ClusterLink: oversized frame (%u bytes); dropping connection", len);
                    m_buf.clear();
                    return;
                }
                if (m_buf.size() - off < ClusterFrame::HEADER_SIZE + len)
                    break; // wait for the rest

                const uint8* payload = p + ClusterFrame::HEADER_SIZE;
                dispatch(type, payload, len);
                off += ClusterFrame::HEADER_SIZE + len;
            }

            if (off)
                m_buf.erase(m_buf.begin(), m_buf.begin() + off);
        }

        // Net-thread dispatch: only cheap/stateless work here; anything touching
        // game state is queued for the world thread.
        void dispatch(uint8 type, const uint8* payload, uint32 len)
        {
            if (type == CLUSTER_MSG_HEARTBEAT)
            {
                uint32 nodeId = 0;
                if (len >= 4)
                    nodeId = (uint32)payload[0] | ((uint32)payload[1] << 8) |
                             ((uint32)payload[2] << 16) | ((uint32)payload[3] << 24);
                sClusterMgr->OnPeerHeartbeat(nodeId);
                return;
            }

            // Everything else is processed on the world thread.
            sClusterMgr->PushInbound(type, payload, len);
        }

        std::vector<uint8> m_buf;
};

// ---------------------------------------------------------------------------
// ClusterThread
// ---------------------------------------------------------------------------
ClusterThread::ClusterThread(uint16 listenPort, const char* host)
    : m_reactor(NULL), m_acceptor(NULL), m_listenAddr(listenPort, host), m_running(true)
{
    ACE_Reactor_Impl* imp = new ACE_TP_Reactor();
    imp->max_notify_iterations(128);
    m_reactor = new ACE_Reactor(imp, 1);
    m_acceptor = new ClusterAcceptor;
}

ClusterThread::~ClusterThread()
{
    for (std::map<uint32, ACE_SOCK_Stream*>::iterator it = m_peerStreams.begin(); it != m_peerStreams.end(); ++it)
    {
        it->second->close();
        delete it->second;
    }
    m_peerStreams.clear();
    delete m_acceptor;
    delete m_reactor;
}

int ClusterThread::open(void* /*unused*/)
{
    if (m_acceptor->open(m_listenAddr, m_reactor, ACE_NONBLOCK) == -1)
    {
        sLog.outError("Cluster: cannot bind inter-node listener to %s:%d",
                      m_listenAddr.get_host_addr(), m_listenAddr.get_port_number());
        return -1;
    }
    activate();
    return 0;
}

void ClusterThread::Stop()
{
    m_running = false;
    if (m_reactor)
        m_reactor->end_reactor_event_loop();
}

void ClusterThread::dropPeer(uint32 nodeId)
{
    std::map<uint32, ACE_SOCK_Stream*>::iterator it = m_peerStreams.find(nodeId);
    if (it == m_peerStreams.end())
        return;
    it->second->close();
    delete it->second;
    m_peerStreams.erase(it);
}

void ClusterThread::ensurePeerConnections()
{
    std::vector<ClusterPeer> peers;
    sClusterMgr->GetPeers(peers);

    for (size_t i = 0; i < peers.size(); ++i)
    {
        ClusterPeer const& peer = peers[i];
        if (m_peerStreams.find(peer.nodeId) != m_peerStreams.end())
            continue;

        ACE_INET_Addr addr((u_short)peer.port, peer.host.c_str());
        ACE_SOCK_Connector connector;
        ACE_SOCK_Stream* stream = new ACE_SOCK_Stream();
        // Short timeout so a batch of down peers can't stall the network thread
        // long. TODO(P2-hardening): replace with a non-blocking connect state
        // machine, and use MSG_NOSIGNAL on send_n to avoid SIGPIPE on Linux.
        ACE_Time_Value timeout(0, 200000); // 200 ms
        if (connector.connect(*stream, addr, &timeout) == -1)
        {
            delete stream; // peer not up yet; try again next tick
            continue;
        }
        m_peerStreams[peer.nodeId] = stream;
        sLog.outString("Cluster: connected outbound to node %u (%s:%u)",
                       peer.nodeId, peer.host.c_str(), peer.port);
    }
}

void ClusterThread::flushOutbound()
{
    std::vector<ClusterOutFrame> frames;
    if (!sClusterMgr->PopOutbound(frames))
        return;

    for (size_t i = 0; i < frames.size(); ++i)
    {
        ClusterOutFrame const& f = frames[i];
        if (f.bytes.empty())
            continue;

        if (f.target == 0)
        {
            // broadcast to every connected peer
            std::vector<uint32> dead;
            for (std::map<uint32, ACE_SOCK_Stream*>::iterator it = m_peerStreams.begin(); it != m_peerStreams.end(); ++it)
            {
                if (it->second->send_n(&f.bytes[0], f.bytes.size()) <= 0)
                    dead.push_back(it->first);
            }
            for (size_t d = 0; d < dead.size(); ++d)
                dropPeer(dead[d]);
        }
        else
        {
            std::map<uint32, ACE_SOCK_Stream*>::iterator it = m_peerStreams.find(f.target);
            if (it != m_peerStreams.end() && it->second->send_n(&f.bytes[0], f.bytes.size()) <= 0)
                dropPeer(f.target);
        }
    }
}

int ClusterThread::svc()
{
    sLog.outString("Cluster: inter-node network thread started (listening on %s:%d)",
                   m_listenAddr.get_host_addr(), m_listenAddr.get_port_number());

    while (m_running && !m_reactor->reactor_event_loop_done())
    {
        ACE_Time_Value interval(0, 100000); // 100 ms reactor slice
        m_reactor->run_reactor_event_loop(interval);

        if (World::IsStopped() || !m_running)
            break;

        ensurePeerConnections();
        flushOutbound();
    }

    m_acceptor->close();
    sLog.outString("Cluster: inter-node network thread stopped");
    return 0;
}
