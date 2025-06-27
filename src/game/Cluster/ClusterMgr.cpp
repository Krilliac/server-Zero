#include "ClusterMgr.h"
#include "Log.h"
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <stdexcept>

ClusterMgr* ClusterMgr::instance()
{
    static ClusterMgr mgr;
    return &mgr;
}

ClusterMgr::ClusterMgr() : m_running(false), m_port(0) {}
ClusterMgr::~ClusterMgr() { Stop(); }

void ClusterMgr::Initialize(const std::string& nodeId, const std::string& bindIp, uint16_t port, 
                            const std::map<std::string, std::pair<std::string, uint16_t>>& peers)
{
    m_nodeId = nodeId;
    m_bindIp = bindIp;
    m_port = port;
    m_peers = peers;
}

void ClusterMgr::Start()
{
    m_running = true;
    m_heartbeatThread = std::thread(&ClusterMgr::HeartbeatThread, this);
    m_listenThread = std::thread(&ClusterMgr::ListenThread, this);
    sLog.outString("Cluster manager started for node: %s", m_nodeId.c_str());
}

void ClusterMgr::Stop()
{
    m_running = false;
    if (m_heartbeatThread.joinable()) m_heartbeatThread.join();
    if (m_listenThread.joinable()) m_listenThread.join();
    sLog.outString("Cluster manager stopped");
}

void ClusterMgr::HeartbeatThread()
{
    while (m_running)
    {
        SendHeartbeats();
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void ClusterMgr::ListenThread()
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        sLog.outError("Cluster: Failed to create UDP socket");
        return;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = inet_addr(m_bindIp.c_str());
    
    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        sLog.outError("Cluster: Failed to bind UDP socket to %s:%d", m_bindIp.c_str(), m_port);
        close(sockfd);
        return;
    }

    char buffer[1024];
    while (m_running)
    {
        sockaddr_in sender;
        socklen_t senderLen = sizeof(sender);
        int len = recvfrom(sockfd, buffer, sizeof(buffer), 0, (sockaddr*)&sender, &senderLen);
        
        if (len > 0)
        {
            try
            {
                ClusterMessage msg = ClusterMessage::Deserialize(
                    reinterpret_cast<const uint8_t*>(buffer), len);
                HandleIncomingMessage(msg);
            }
            catch (const std::exception& e)
            {
                sLog.outError("Cluster: Failed to deserialize message: %s", e.what());
            }
        }
        else if (len < 0)
        {
            sLog.outError("Cluster: Socket recv error: %s", strerror(errno));
        }
    }
    close(sockfd);
}

void ClusterMgr::SendHeartbeats()
{
    ClusterMessage msg;
    msg.type = CLUSTER_MSG_HEARTBEAT;
    msg.sourceNode = m_nodeId;
    auto serialized = msg.Serialize();
    
    for (const auto& [peerId, peerInfo] : m_peers)
    {
        if (peerId == m_nodeId) continue;
        SendToAddress(peerInfo.first, peerInfo.second, 
                      serialized.data(), serialized.size());
    }
}

// Method for sending raw data
void ClusterMgr::SendToAddress(const std::string& ip, uint16_t port, const void* data, size_t size) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        sLog.outError("Cluster: Failed to create socket for %s:%d", ip.c_str(), port);
        return;
    }
    
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    
    if (sendto(sockfd, data, size, 0, (sockaddr*)&addr, sizeof(addr)) < 0) {
        sLog.outError("Cluster: Failed to send data to %s:%d: %s", 
                     ip.c_str(), port, strerror(errno));
    }
    close(sockfd);
}

void ClusterMgr::SendToNode(const std::string& nodeId, const ClusterMessage& msg)
{
    auto it = m_peers.find(nodeId);
    if (it == m_peers.end()) 
    {
        sLog.outError("Cluster: Unknown node %s", nodeId.c_str());
        return;
    }
    
    auto serialized = msg.Serialize();
    SendToAddress(it->second.first, it->second.second, 
                  serialized.data(), serialized.size());
}

void ClusterMgr::BroadcastMessage(const ClusterMessage& msg)
{
    auto serialized = msg.Serialize();
    for (const auto& [peerId, peerInfo] : m_peers)
    {
        if (peerId == m_nodeId) continue;
        SendToAddress(peerInfo.first, peerInfo.second, 
                      serialized.data(), serialized.size());
    }
}

void ClusterMgr::RegisterHandler(uint8_t type, MessageHandler handler)
{
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    m_messageHandlers[type] = handler;
}

void ClusterMgr::HandleIncomingMessage(const ClusterMessage& msg)
{
    if (msg.type == CLUSTER_MSG_HEARTBEAT)
    {
        MarkNodeAlive(msg.sourceNode);
        sLog.outDebug("Cluster: Received heartbeat from %s", msg.sourceNode.c_str());
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    auto it = m_messageHandlers.find(msg.type);
    if (it != m_messageHandlers.end())
    {
        it->second(msg);
    }
    else
    {
        sLog.outError("Cluster: No handler for message type %d", static_cast<int>(msg.type));
    }
}

void ClusterMgr::MarkNodeAlive(const std::string& nodeId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastHeartbeat[nodeId] = std::chrono::steady_clock::now();
}

bool ClusterMgr::IsNodeAlive(const std::string& nodeId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_lastHeartbeat.find(nodeId);
    if (it == m_lastHeartbeat.end()) return false;
    
    auto now = std::chrono::steady_clock::now();
    return (now - it->second) < std::chrono::seconds(6);
}