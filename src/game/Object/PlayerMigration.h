// PlayerMigration.h
#pragma once
#include "ByteBuffer.h"
#include "ObjectGuid.h"
#include "Auth/Sha1.h"
#include <vector>
#include <array>
#include <string>
#include <utility> // For std::pair

struct EquipmentItemData {
    bool exists = false;
    uint32 entry = 0;
    uint32 count = 0;
    uint32 enchantId = 0;
};

struct AuraMigrationData {
    uint32 spellId;
    uint32 stacks;
    uint32 duration;
};

struct QuestData {
    uint32 questId;
    uint8 status;
    bool rewarded;
    std::vector<std::pair<uint32, uint32>> mobCounters; // (entry, count)
};

struct ReputationData {
    uint32 factionId;
    int32 standing; // Can be negative
    uint32 flags;
};

struct PlayerMigrationData {
    static const uint16 CURRENT_VERSION = 1;
    
    ObjectGuid guid;
    std::string name;
    uint32 accountId;
    
    // Position
    float positionX, positionY, positionZ, orientation;
    uint32 mapId, zoneId, areaId;
    
    // Stats
    uint32 health, mana, level, race, class_, gender;
    uint32 xp, money, totalHonor, arenaPoints, totalKills;
    uint32 maxHealth, maxMana, equipmentSetId;
    
    // Equipment
    std::array<EquipmentItemData, 19> equipment;
    
    // Spells and auras
    std::vector<uint32> spells;
    std::vector<AuraMigrationData> auras;
    
    // Quests
    std::vector<QuestData> quests;
    
    // Reputation
    std::vector<ReputationData> reputations;
    
    // Action bars - MAX_ACTION_BAR_INDEX = 120
    std::array<uint32, 120> actionBars;
    
    // Skills - (skillId, value)
    std::vector<std::pair<uint16, uint16>> skills;
    
    // Movement info
    uint32 movementFlags;
    ObjectGuid transportGuid;
    float transportPosX, transportPosY, transportPosZ, transportPosO;
    uint32 fallTime;
    
    void Serialize(ByteBuffer& buffer) const;
    bool Deserialize(ByteBuffer& buffer);
    
private:
    void ComputeChecksum(const uint8* data, size_t len, uint8* out) const;
    bool VerifyChecksum(ByteBuffer& buffer, size_t dataSize) const;
};