#pragma once
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include "ClusterMessage.h"

class ClusterMgr {
public:
    using MessageHandler = std::function<void(const ClusterMessage&)>;
    
    static ClusterMgr* instance();
    
    void Initialize(const std::string& nodeId, const std::string& bindIp, uint16_t port, 
                   const std::map<std::string, std::pair<std::string, uint16_t>>& peers);
    void Start();
    void Stop();
    
    bool IsNodeAlive(const std::string& nodeId) const;
    const std::map<std::string, std::pair<std::string, uint16_t>>& GetPeers() const { return m_peers; }
    std::string GetNodeId() const { return m_nodeId; }
    
    void RegisterHandler(uint8_t type, MessageHandler handler);
    void SendToNode(const std::string& nodeId, const ClusterMessage& msg);
    void BroadcastMessage(const ClusterMessage& msg);
    void SendToAddress(const std::string& ip, uint16_t port, const void* data, size_t size);

private:
    ClusterMgr();
    ~ClusterMgr();

    void HeartbeatThread();
    void ListenThread();
    void HandleIncomingMessage(const ClusterMessage& msg);
    void MarkNodeAlive(const std::string& nodeId);
    
    std::string m_nodeId;
    std::string m_bindIp;
    uint16_t m_port;
    std::map<std::string, std::pair<std::string, uint16_t>> m_peers;
    std::map<std::string, std::chrono::steady_clock::time_point> m_lastHeartbeat;
    std::atomic<bool> m_running;
    std::thread m_heartbeatThread;
    std::thread m_listenThread;
    std::unordered_map<uint8_t, MessageHandler> m_messageHandlers;
    mutable std::mutex m_mutex;
    mutable std::mutex m_handlerMutex;
};

#define sClusterMgr ClusterMgr::instance()