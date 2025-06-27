#include "World.h"
#include "MapManager.h"
#include "Player.h"
#include "Transport.h"
#include "Log.h"
#include "Cluster/ClusterMgr.h"
#include "Config/Config.h"
#include "MetricTracker.h"
#include "AntiCheatMgr.h"
#include "TimeSyncMgr.h"
#include "Network/NetworkHandler.h"
#include "Hivemind.h"  // For node management
#include "Cluster/ClusterMessage.h"  // For migration handler

// Cluster check interval (10 seconds)
constexpr uint32 CLUSTER_CHECK_INTERVAL = 10000;

World* World::instance()
{
    static World instance;
    return &instance;
}

World::World() : 
    m_lastWorldUpdate(0),
    m_clusterCheckTimer(0),
    m_avgPhysicsError(0.0f)
{
    // Initialize down nodes set
    m_downNodes.clear();
}

void World::Initialize()
{
    // Initialize core systems
    sHivemind->Initialize(MAIN_WORLD_NODE);
    sMapMgr->Initialize();
    sTransportMgr->Initialize();
    sTimeSyncMgr->Initialize(); // Initialize time sync system
    sNetworkHandler->Initialize();
    
    if (sConfig.GetBoolDefault("Cluster.Enabled", false))
    {
        std::string nodeId = sConfig.GetStringDefault("Cluster.NodeID", "world1");
        std::string bindIp = sConfig.GetStringDefault("Cluster.BindIP", "127.0.0.1");
        uint16_t port = sConfig.GetIntDefault("Cluster.Port", 5000);

        std::map<std::string, std::pair<std::string, uint16_t>> peers;
        std::string peersStr = sConfig.GetStringDefault("Cluster.Peers", "");
        
        // Split peer list string
        std::vector<std::string> peerList = StrSplit(peersStr, ",");
        for (const auto& peer : peerList)
        {
            if (peer.empty()) continue;
            
            // Split each peer into nodeId:ip:port
            std::vector<std::string> parts = StrSplit(peer, ":");
            if (parts.size() == 3)
            {
                try {
                    // Use parts[0] for nodeID, parts[1] for IP, parts[2] for port
                    peers[parts[0]] = std::make_pair(
                        parts[1], 
                        static_cast<uint16_t>(std::stoi(parts[2]))
                    );
                }
                catch (const std::exception& e) {
                    sLog.outError("Invalid port for peer %s: %s", parts[0].c_str(), e.what());
                }
            }
            else
            {
                sLog.outError("Invalid peer format: %s (expected nodeId:ip:port)", peer.c_str());
            }
        }

        sClusterMgr->Initialize(nodeId, bindIp, port, peers);
        sClusterMgr->Start();
        sLog.outString("Cluster manager started for node: %s", nodeId.c_str());
        
        // Register cluster player migration handler
        sClusterMgr->RegisterHandler(CLUSTER_MSG_PLAYER_MIGRATION, [this](const ClusterMessage& msg) {
            WorldPacket data(msg.payload.data(), msg.payload.size());
            
            // Deserialize core state
            ObjectGuid guid;
            std::string name;
            float x, y, z, o;
            uint32 mapId, zoneId, areaId;
            uint32 health, mana, level, race, class_, gender;
            uint32 xp, money, totalHonor, arenaPoints, totalKills;
            uint32 maxHealth, maxMana, equipmentSetId;
            
            data >> guid >> name >> x >> y >> z >> o >> mapId >> zoneId >> areaId;
            data >> health >> mana >> level >> race >> class_ >> gender;
            data >> xp >> money >> totalHonor >> arenaPoints >> totalKills;
            data >> maxHealth >> maxMana >> equipmentSetId;
            
            // Create migration session
            WorldSession* session = new WorldSession(0, nullptr, SEC_PLAYER);
            session->SetMigrationState(true);
            
            // Create player
            Player* player = new Player(session);
            player->SetGuid(guid); // Requires SetGuid in Object class
            player->SetName(name);
            player->Relocate(x, y, z, o);
            player->SetMapId(mapId);
            player->SetZoneId(zoneId);
            player->SetAreaId(areaId);
            player->SetHealth(health);
            player->SetPower(POWER_MANA, mana);
            player->SetLevel(level);
            player->SetRace(race);
            player->SetClass(class_);
            player->SetGender(gender);
            player->SetXP(xp);
            player->SetMoney(money);
            player->SetTotalHonorPoints(totalHonor);
            player->SetArenaPoints(arenaPoints);
            player->SetTotalKills(totalKills);
            player->SetMaxHealth(maxHealth);
            player->SetMaxPower(POWER_MANA, maxMana);
            player->SetEquipmentSetId(equipmentSetId);
            
            // Deserialize equipment
            for (uint8 i = 0; i < EQUIPMENT_SLOT_END; i++)
            {
                uint8 itemExists;
                data >> itemExists;
                if (itemExists)
                {
                    uint32 entry, count, enchantId;
                    data >> entry >> count >> enchantId;
                    
                    if (Item* item = Item::CreateItem(entry, count, player))
                    {
                        if (enchantId)
                            item->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantId, 0, 0);
                        player->EquipItem(i, item);
                    }
                }
            }
            
            // Deserialize spells
            uint32 spellCount;
            data >> spellCount;
            for (uint32 i = 0; i < spellCount; i++)
            {
                uint32 spellId;
                data >> spellId;
                player->LearnSpell(spellId, false);
            }
            
            // Deserialize auras
            uint32 auraCount;
            data >> auraCount;
            for (uint32 i = 0; i < auraCount; i++)
            {
                uint32 spellId, stacks, duration;
                data >> spellId >> stacks >> duration;
                player->AddAura(spellId, player, nullptr, duration, stacks);
            }
            
            // Finalize player
            session->SetPlayer(player);
            player->AddToWorld();
            sLog.outString("Migrated player %s to node %s", name.c_str(), sClusterMgr->GetNodeId().c_str());
        });
    }
    
    // Load configuration
    ReloadConfig();
    
    sLog.outString("World initialized");
}

void World::Update(uint32_t diff)
{
    DynamicTick::Update(diff);
    const uint32_t interval = DynamicTick::GetCurrentTickInterval();
    
    m_lastWorldUpdate += diff;
    while (m_lastWorldUpdate >= interval)
    {
        // Core world updates
        UpdateMaps(interval);
        UpdatePlayers(interval);
        UpdateTransports(interval);
        
        // Cluster updates
        sHivemind->Update(interval);
        sTimeSyncMgr->Update(interval);
        
        m_lastWorldUpdate -= interval;
        
        // Update physics metrics
        m_avgPhysicsError = 0.9f * m_avgPhysicsError + 0.1f * CalculateAvgPhysicsError();
        sMetricTracker.RecordPhysicsError(m_avgPhysicsError);
    }
    
    // Update network handler with actual diff
    sNetworkHandler->Update(diff);
    
    // Cluster node liveness check (every 10 seconds)
    m_clusterCheckTimer += diff;
    if (m_clusterCheckTimer >= CLUSTER_CHECK_INTERVAL)
    {
        CheckClusterState();
        m_clusterCheckTimer = 0;
    }
}

void World::CheckClusterState()
{
    if (!sConfig.GetBoolDefault("Cluster.Enabled", false))
        return;

    const auto& peers = sClusterMgr->GetPeers();
    for (const auto& [nodeId, addr] : peers)
    {
        if (!sClusterMgr->IsNodeAlive(nodeId))
        {
            // New node failure detected
            if (m_downNodes.find(nodeId) == m_downNodes.end())
            {
                sLog.outError("Cluster: Node %s is down", nodeId.c_str());
                HandleNodeFailure(nodeId);
                m_downNodes.insert(nodeId);
            }
        }
        else
        {
            // Node recovered
            if (m_downNodes.find(nodeId) != m_downNodes.end())
            {
                sLog.outString("Cluster: Node %s is back online", nodeId.c_str());
                HandleNodeRecovery(nodeId);
                m_downNodes.erase(nodeId);
            }
        }
    }
}

void World::HandleNodeFailure(const std::string& nodeId)
{
    uint32 migratedPlayers = 0;
    
    // Reassign players from failed node
    for (auto& [guid, session] : m_sessions)
    {
        Player* player = session->GetPlayer();
        if (player && player->GetNodeId() == nodeId)
        {
            uint32_t newNode = sHivemind->GetOptimalNodeFor(player);
            if (newNode != player->GetNodeId())
            {
                sHivemind->MigratePlayer(player, newNode);
                migratedPlayers++;
            }
        }
    }
    
    // Reassign world resources (zones, instances, etc.)
    sHivemind->ReassignNodeResources(nodeId);
    sLog.outString("Migrated %u players from failed node %s", migratedPlayers, nodeId.c_str());
}

void World::HandleNodeRecovery(const std::string& nodeId)
{
    // Reactivate node resources
    sHivemind->ActivateNode(nodeId);
    
    // Optional: Rebalance players back to recovered node
    if (sConfig.GetBoolDefault("Cluster.AutoRebalance", true))
    {
        uint32 rebalanced = sHivemind->RebalancePlayers();
        sLog.outString("Rebalanced %u players to node %s", rebalanced, nodeId.c_str());
    }
}

void World::UpdatePlayers(uint32_t diff)
{
    for (auto& [guid, session] : m_sessions)
    {
        Player* player = session->GetPlayer();
        if (player)
        {
            // Update player state
            player->Update(diff);
            
            // Auto-migration check
            if (sConfig.GetBoolDefault("Cluster.EnableAutoMigration", true))
            {
                if (player->GetPosition().DistanceToSquared(player->m_lastNodeCheckPos) > 
                    sConfig.GetFloatDefault("Cluster.MigrationDistance", 10000.0f))
                {
                    uint32_t newNode = sHivemind->GetOptimalNodeFor(player);
                    if (newNode != player->GetNodeId())
                    {
                        sHivemind->MigratePlayer(player, newNode);
                        player->m_lastNodeCheckPos = player->GetPosition();
                    }
                }
            }
        }
    }
}

void World::UpdateMaps(uint32_t diff)
{
    sMapMgr->Update(diff);
}

void World::UpdateTransports(uint32_t diff)
{
    sTransportMgr->Update(diff);
}

uint32_t World::GetActiveSessionCount() const
{
    return m_sessions.size();
}

uint32_t World::GetPlayerCount() const
{
    uint32_t count = 0;
    for (const auto& [guid, session] : m_sessions)
    {
        if (session->GetPlayer())
            count++;
    }
    return count;
}

void World::AddNode(WorldNode* node)
{
    sHivemind->AddNode(node);
}

void World::RemoveNode(uint32_t nodeId)
{
    sHivemind->RemoveNode(nodeId);
}

void World::ReloadConfig()
{
    // Reinitialize systems with new config
    sTimeSyncMgr->Initialize();
    sAntiCheatMgr->Initialize();
    DynamicTick::Initialize();
}

float World::CalculateAvgPhysicsError() const
{
    if (m_sessions.empty()) 
        return 0.0f;
        
    float totalError = 0.0f;
    uint32_t playerCount = 0;
    
    for (const auto& [guid, session] : m_sessions)
    {
        if (Player* player = session->GetPlayer())
        {
            totalError += player->GetPhysicsError();
            playerCount++;
        }
    }
    
    return playerCount > 0 ? totalError / playerCount : 0.0f;
}