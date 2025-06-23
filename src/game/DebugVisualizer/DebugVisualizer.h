#pragma once
#include "Player.h"
#include "ClusterVisuals.h"
#include "AntiCheatVisuals.h"
#include "SyncVisualizer.h"
#include "WardenVisualizer.h"
#include "MapGridVisualizer.h"
#include "RespawnVisualizer.h"
#include "NetworkPathVisualizer.h"
#include "LootVisualizer.h"
#include "PathVisualizer.h"
#include "AIVisualizer.h"
#include "SpellVisualizer.h"
#include "ThreatVisualizer.h"
#include "SpellRadiusVisualizer.h"
#include "LoSVisualizer.h"
#include "QuestVisualizer.h"
#include "CombatVisuals.h"
#include <vector>

class DebugVisualizer
{
public:
    // Core visualizations
    static void ShowPath(Player* player, const std::vector<Position>& path);
    static void ShowAIState(Player* player, const std::string& aiState);
    static void ShowSyncStatus(Player* player, float drift, float latency);
    
    // Cluster system
    static void ShowMigration(Player* player, uint32 fromNode, uint32 toNode) {
        ClusterVisuals::ShowMigration(player, fromNode, toNode);
    }
    
    static void ShowNodeStatus(Player* gm) {
        ClusterVisuals::ShowNodeStatus(gm);
    }
    
    // Anti-cheat system
    static void ShowAntiCheatViolation(Player* player, const std::string& reason) {
        AntiCheatVisuals::ShowViolation(player, reason);
    }
    
    static void ShowSpeedHack(Player* player, float speedRatio) {
        AntiCheatVisuals::ShowSpeedHack(player, speedRatio);
    }
    
    // Time sync system
    static void ShowDrift(Player* player, float drift, float latency) {
        SyncVisualizer::ShowDrift(player, drift, latency);
    }
    
    // Warden system
    static void ShowWardenViolation(Player* player, int type) {
        WardenVisualizer::ShowViolation(player, type);
    }
    
    // Map system
    static void ShowGrid(Player* player) {
        MapGridVisualizer::ShowGrid(player);
    }
    
    // Respawn system
    static void ShowRespawnTimer(Player* player, GameObject* obj, uint32 respawnTime) {
        RespawnVisualizer::ShowRespawnTimer(player, obj, respawnTime);
    }
    
    // Network system
    static void ShowPacketRoute(Player* player, const std::vector<Position>& path) {
        NetworkPathVisualizer::ShowPacketRoute(player, path);
    }
    
    // Loot system
    static void ShowLootTable(Player* player, WorldObject* source) {
        LootVisualizer::ShowLootTable(player, source);
    }
    
    // Pathfinding
    static void ShowCreaturePath(Player* player, Creature* creature) {
        PathVisualizer::ShowCreaturePath(player, creature);
    }
    
    // AI system
    static void ShowThreatList(Player* player, Unit* target) {
        AIVisualizer::ShowThreatList(player, target);
    }
    
    // Spell system
    static void ShowSpellTarget(Player* player, Spell* spell) {
        SpellVisualizer::ShowSpellTarget(player, spell);
    }
    
    // Threat system
    static void ShowThreatDirection(Player* player, Unit* source, Unit* target) {
        ThreatVisualizer::ShowThreatDirection(player, source, target);
    }
    
    // Spell radii
    static void ShowSpellRadius(Player* player, const Position& center, float radius) {
        SpellRadiusVisualizer::ShowSpellRadius(player, center, radius);
    }
    
    // Line of sight
    static void ShowLineOfSight(Player* player, Unit* source, Unit* target) {
        LoSVisualizer::ShowLineOfSight(player, source, target);
    }
    
    // Quest system
    static void ShowQuestArea(Player* player, uint32 questId, const Position& center, float radius) {
        QuestVisualizer::ShowQuestArea(player, questId, center, radius);
    }
    
    // Combat system
    static void ShowCombatRange(Player* player, Unit* attacker, Unit* target) {
        CombatVisuals::ShowCombatRange(player, attacker, target);
    }
    
    // Additional visualizations
    static void ShowClusterNode(Player* player, uint32 nodeId, uint32 playerCount);
    static void ShowNetworkRoute(Player* player, const std::vector<uint32>& hops);
};
