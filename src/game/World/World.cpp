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
#include "Hivemind.h"
#include "Cluster/ClusterMessage.h"
#include "PlayerMigration.h"

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
    m_downNodes.clear();
}

void World::Initialize()
{
    sHivemind->Initialize(MAIN_WORLD_NODE);
    sMapMgr->Initialize();
    sTransportMgr->Initialize();
    sTimeSyncMgr->Initialize();
    sNetworkHandler->Initialize();
    
    if (sConfig.GetBoolDefault("Cluster.Enabled", false))
    {
        std::string nodeId = sConfig.GetStringDefault("Cluster.NodeID", "world1");
        std::string bindIp = sConfig.GetStringDefault("Cluster.BindIP", "127.0.0.1");
        uint16_t port = static_cast<uint16_t>(sConfig.GetIntDefault("Cluster.Port", 5000));

        std::map<std::string, std::pair<std::string, uint16_t>> peers;
        std::string peersStr = sConfig.GetStringDefault("Cluster.Peers", "");
        
        if (!peersStr.empty()) {
            std::vector<std::string> peerList = StrSplit(peersStr, ",");
            for (const auto& peer : peerList)
            {
                if (peer.empty()) continue;
                
                std::vector<std::string> parts = StrSplit(peer, ":");
                if (parts.size() == 3)
                {
                    try {
                        uint16_t peerPort = static_cast<uint16_t>(std::stoi(parts[2]));
                        peers[parts[0]] = std::make_pair(parts[1], peerPort);
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
        }

        sClusterMgr->Initialize(nodeId, bindIp, port, peers);
        sClusterMgr->Start();
        sLog.outString("Cluster manager started for node: %s", nodeId.c_str());
        
        sClusterMgr->RegisterHandler(CLUSTER_MSG_PLAYER_MIGRATION, [this](const ClusterMessage& msg) {
            try {
                PlayerMigrationData data;
                ByteBuffer buffer(msg.payload.data(), msg.payload.size());
                if (!data.Deserialize(buffer)) {
                    sLog.outError("Player migration failed: Deserialization error");
                    return;
                }

                // Create migration session
                WorldSession* session = new WorldSession(data.accountId, nullptr, SEC_PLAYER);
                session->SetMigrationState(true);
                
                // Create player object
                Player* player = new Player(session);
                player->SetGuid(data.guid);
                player->SetName(data.name);
                player->Relocate(data.positionX, data.positionY, data.positionZ, data.orientation);
                player->SetMapId(data.mapId);
                player->SetZoneId(data.zoneId);
                player->SetAreaId(data.areaId);

                // Core stats
                player->SetHealth(data.health);
                player->SetPower(POWER_MANA, data.mana);
                player->SetLevel(data.level);
                player->SetRace(data.race);
                player->SetClass(data.class_);
                player->SetGender(data.gender);

                // Progression
                player->SetXP(data.xp);
                player->SetMoney(data.money);
                player->SetTotalHonorPoints(data.totalHonor);
                player->SetArenaPoints(data.arenaPoints);
                player->SetTotalKills(data.totalKills);

                // Attributes
                player->SetMaxHealth(data.maxHealth);
                player->SetMaxPower(POWER_MANA, data.maxMana);
                player->SetEquipmentSetId(data.equipmentSetId);

                // Equipment
                for (uint8 i = 0; i < data.equipment.size(); ++i) {
                    if (data.equipment[i].exists) {
                        Item* item = Item::CreateItem(
                            data.equipment[i].entry,
                            data.equipment[i].count,
                            player
                        );
                        if (item) {
                            if (data.equipment[i].enchantId) {
                                item->SetEnchantment(
                                    PERM_ENCHANTMENT_SLOT,
                                    data.equipment[i].enchantId,
                                    0, 0
                                );
                            }
                            player->EquipItem(i, item);
                        }
                    }
                }

                // Spells
                for (uint32 spellId : data.spells) {
                    player->LearnSpell(spellId, false);
                }

                // Auras
                for (const auto& aura : data.auras) {
                    player->AddAura(
                        aura.spellId,
                        player,
                        nullptr,
                        aura.duration,
                        aura.stacks
                    );
                }

                // Quests
                for (auto& quest : data.quests) {
                    player->SetQuestStatus(quest.questId, QuestStatus(quest.status));
                    if (quest.rewarded) {
                        player->SetQuestRewarded(quest.questId);
                    }
                    for (auto& counter : quest.mobCounters) {
                        player->SetQuestSlotCounter(quest.questId, counter.first, counter.second);
                    }
                }

                // Reputation
                for (auto& rep : data.reputations) {
                    player->SetReputation(rep.factionId, rep.standing);
                    player->SetReputationFlags(rep.factionId, rep.flags);
                }

                // Action bars
                for (uint8 i = 0; i < data.actionBars.size(); i++) {
                    player->SetActionButton(i, data.actionBars[i]);
                }

                // Skills
                for (auto& skill : data.skills) {
                    player->SetSkill(skill.first, 1, skill.second, skill.second);
                }

                // Movement info
                player->SetMovementFlags(data.movementFlags);
                if (data.movementFlags & MOVEFLAG_ONTRANSPORT) {
                    player->SetTransportData(
                        data.transportGuid,
                        data.transportPosX,
                        data.transportPosY,
                        data.transportPosZ,
                        data.transportPosO
                    );
                }
                player->SetFallTime(data.fallTime);

                // Finalize player
                session->SetPlayer(player);
                player->AddToWorld();

                sLog.outString("Migrated player %s to node %s", 
                              data.name.c_str(), 
                              sClusterMgr->GetNodeId().c_str());
            }
            catch (const std::exception& e) {
                sLog.outError("Player migration failed: %s", e.what());
            }
        });
    }
    
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
        UpdateMaps(interval);
        UpdatePlayers(interval);
        UpdateTransports(interval);
        sHivemind->Update(interval);
        sTimeSyncMgr->Update(interval);
        
        m_lastWorldUpdate -= interval;
        m_avgPhysicsError = 0.9f * m_avgPhysicsError + 0.1f * CalculateAvgPhysicsError();
        sMetricTracker.RecordPhysicsError(m_avgPhysicsError);
    }
    
    sNetworkHandler->Update(diff);
    
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
            if (m_downNodes.find(nodeId) == m_downNodes.end())
            {
                sLog.outError("Cluster: Node %s is down", nodeId.c_str());
                HandleNodeFailure(nodeId);
                m_downNodes.insert(nodeId);
            }
        }
        else
        {
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
    
    sHivemind->ReassignNodeResources(nodeId);
    sLog.outString("Migrated %u players from failed node %s", migratedPlayers, nodeId.c_str());
}

void World::HandleNodeRecovery(const std::string& nodeId)
{
    sHivemind->ActivateNode(nodeId);
    
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
            player->Update(diff);
            
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