#include "NetworkHandler.h"
#include "WorldSession.h"
#include "Log.h"
#include "Config/Config.h"
#include "Util.h"
#include "Auth/CryptoHash.h"
#include "ZstdCompression.h"
#include "WorldSocket.h"
#include <vector>
#include <fstream>
#include <sstream>

NetworkHandler* NetworkHandler::instance()
{
    static NetworkHandler instance;
    return &instance;
}

void NetworkHandler::Initialize()
{
    m_enableUDP = sConfig.GetBoolDefault("Network.EnableUDP", false);
    m_compressionThreshold = sConfig.GetIntDefault("Network.CompressionThreshold", 128);
    
    if (m_enableUDP)
    {
        InitializeUDPSocket();
        sLog.outString("UDP socket initialized on port %d", m_udpPort);
    }
    
    LoadEncryptionKeys();
}

void NetworkHandler::Update(uint32 diff)
{
    if (m_enableUDP)
    {
        ProcessUDPQueue();
    }
    
    // Bandwidth throttling update
    UpdateBandwidthLimits(diff);
    
    // Security audit every 5 minutes
    static uint32 auditTimer = 0;
    auditTimer += diff;
    if (auditTimer >= 300000) // 5 minutes
    {
        sSecurityMgr->AuditNetworkTraffic();
        auditTimer = 0;
    }
}

WorldPacket NetworkHandler::CompressPacket(const WorldPacket& source)
{
    return ZstdCompression::Compress(source);
}

bool NetworkHandler::ValidatePacket(WorldSession* session, WorldPacket& packet)
{
    // Validate packet size
    if (packet.size() > MAX_PACKET_SIZE)
    {
        sLog.outError("Oversized packet from %s", session->GetPlayerName());
        return false;
    }
    
    // Validate encryption
    if (!ValidateEncryption(session, packet))
    {
        sSecurityMgr->RecordViolation(session->GetPlayer(), 
            VIOLATION_ENCRYPTION, "Invalid packet encryption");
        return false;
    }
    
    return true;
}

// ================= PRIVATE IMPLEMENTATION ================= //

void NetworkHandler::InitializeUDPSocket()
{
    // OS-specific UDP socket initialization
    #ifdef WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
    #endif

    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    
    sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_udpPort);
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    
    bind(m_udpSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
    
    // Set non-blocking
    #ifdef WIN32
    u_long mode = 1;
    ioctlsocket(m_udpSocket, FIONBIO, &mode);
    #else
    fcntl(m_udpSocket, F_SETFL, O_NONBLOCK);
    #endif
}

void NetworkHandler::ProcessUDPQueue()
{
    sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    char buffer[MAX_PACKET_SIZE];
    
    while (true)
    {
        int bytesReceived = recvfrom(m_udpSocket, buffer, MAX_PACKET_SIZE, 0,
                                    (sockaddr*)&clientAddr, &addrLen);
        
        if (bytesReceived <= 0)
            break;
            
        HandleUDPPacket(buffer, bytesReceived, clientAddr);
    }
}

void NetworkHandler::HandleUDPPacket(const char* data, size_t size, sockaddr_in& fromAddr)
{
    // Process UDP movement packets
    WorldPacket packet(MSG_MOVE_HEARTBEAT, data, size);
    
    if (Player* player = FindPlayerByIP(fromAddr))
    {
        if (sConfig.GetBoolDefault("Network.UDPFastPath", true))
        {
            sOptimizedSocket->ProcessUDP(packet);
        }
    }
}

void NetworkHandler::UpdateBandwidthLimits(uint32 diff)
{
    // Update bandwidth counters for each session
    for (auto& session : sWorld->getSessions())
    {
        session.second->UpdateBandwidthUsage(diff);
        
        // Enforce bandwidth limits
        if (session.second->GetBandwidthUsage() > MAX_BANDWIDTH_PER_INTERVAL)
        {
            session.second->ReduceBandwidth();
        }
    }
}

// Encryption key handling summary:
// - Encrypts packet headers to prevent tampering and sniffing.
// - Key is derived from secure login (SRP6) and is unique per session.
// - Ensures only authenticated clients can communicate with the server.
// - Helps prevent replay attacks, session hijacking, and most bots/hacks.
// - Good key management (secure generation, storage, rotation) is essential for server security.
// - Use openSSL for production keys

// Encryption Key Generation Instructions for Server Administrators
//
// To generate a secure 256-bit (32-byte) encryption key, use the following commands:
//
// On Linux/macOS (bash):
//   openssl rand -base64 32 > conf/encryption.keys
//
// On Windows (PowerShell):
//   openssl rand -base64 32 > conf\encryption.keys
//
// Alternatively, to generate a hex-encoded key:
//
// On Linux/macOS (bash):
//   openssl rand -hex 32 > conf/encryption.keys
//
// On Windows (PowerShell):
//   openssl rand -hex 32 > conf\encryption.keys
//
// Notes:
// - Base64 keys are commonly used and compatible with most server configurations.
// - Hex keys are easier to read and can be used if desired.
// - Ensure the key file is stored securely with appropriate permissions.
// - Use the generated key file path in your server configuration (e.g., mangosd.conf).

void NetworkHandler::LoadEncryptionKeys()
{
    m_encryptionKeys.clear();

    // 1. Load from config
    std::string configKeys = sConfig.GetStringDefault("Network.EncryptionKeys", "");
    if (!configKeys.empty())
    {
        Tokens keys = StrSplit(configKeys, ";");
        for (const auto& key : keys)
        {
            if (!key.empty())
            {
                m_encryptionKeys.push_back(key);
                sLog.outDebug("Loaded encryption key from config: %s", key.c_str());
            }
        }
    }

    // 2. Load from file if not found in config
    if (m_encryptionKeys.empty())
    {
        std::string keyFile = sConfig.GetStringDefault("Network.EncryptionKeyFile", "encryption.keys");
        std::ifstream file(keyFile);
        if (file.is_open())
        {
            std::string line;
            while (std::getline(file, line))
            {
                // Skip comments and empty lines
                if (line.empty() || line[0] == '#') continue;
                line.erase(std::remove_if(line.begin(), line.end(), ::isspace), line.end());
                if (!line.empty())
                {
                    m_encryptionKeys.push_back(line);
                    sLog.outDebug("Loaded encryption key from file: %s", line.c_str());
                }
            }
            file.close();
        }
        else
        {
            sLog.outError("Failed to open encryption key file: %s", keyFile.c_str());
        }
    }

    // 3. Generate default key if none loaded
    if (m_encryptionKeys.empty())
    {
        std::string rawKey = sConfig.GetStringDefault("WorldServerName", "MaNGOS") + std::to_string(time(nullptr));
        uint8 digest[32];
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, rawKey.data(), rawKey.size());
        SHA256_Final(digest, &ctx);

        std::string defaultKey = ByteArrayToHexStr(digest, 32); // Use existing util
        m_encryptionKeys.push_back(defaultKey);
        sLog.outBasic("Generated default encryption key: %s", defaultKey.c_str());
    }

    sLog.outString("Loaded %u encryption keys", uint32(m_encryptionKeys.size()));
}

std::string NetworkHandler::GenerateDefaultKey() const
{
    // Generate SHA-256 hash of server name + timestamp as default key
    std::string rawKey = sConfig.GetStringDefault("WorldServerName", "MaNGOS") + 
                         std::to_string(time(nullptr));
    
    uint8 digest[32];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, rawKey.data(), rawKey.size());
    SHA256_Final(digest, &ctx);
    
    return ByteArrayToHexStr(digest, 32);
}

bool NetworkHandler::IsValidBase64(const std::string& str) const
{
    // Basic base64 validation
    const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/=";
    
    return !str.empty() &&
           str.size() % 4 == 0 &&
           std::all_of(str.begin(), str.end(), [&](char c) {
               return base64_chars.find(c) != std::string::npos;
           });
}