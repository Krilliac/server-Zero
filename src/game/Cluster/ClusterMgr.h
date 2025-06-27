#pragma once
#include "ClusterMessage.h"
#include "Log.h"
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

class ClusterMgr {
public:
    using MessageHandler = std::function<void(const ClusterMessage&)>;
    
    static ClusterMgr* instance();
    
    void Initialize(const std::string& nodeId, const std::string& bindIp, uint16_t port, 
                   const std::map<std::string, std::pair<std::string, uint16_t>>& peers);
    void Start();
    void Stop();
    
    bool IsNodeAlive(const std::string& nodeId) const;
	bool IsNodeValid(uint32 nodeId) const {
    return m_nodes.find(nodeId) != m_nodes.end();
	}
    const std::map<std::string, std::pair<std::string, uint16_t>>& GetPeers() const { return m_peers; }
    std::string GetNodeId() const { return m_nodeId; }
    
    void RegisterHandler(uint8_t type, MessageHandler handler);
    void SendToNode(const std::string& nodeId, const ClusterMessage& msg);
    void BroadcastMessage(const ClusterMessage& msg);
    
private:
    ClusterMgr();
    ~ClusterMgr();

    void HeartbeatThread();
    void ListenThread();
    void HandleIncomingMessage(const ClusterMessage& msg);
    void MarkNodeAlive(const std::string& nodeId);
    void SendToAddress(const std::string& ip, uint16_t port, const void* data, size_t size);
    void CloseSocket(int sockfd);
    
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
    
    #ifdef _WIN32
    WSADATA m_wsaData;
    #endif
};

#define sClusterMgr ClusterMgr::instance()