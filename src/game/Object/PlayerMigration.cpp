#include "PlayerMigration.h"
#include "Log.h"

void PlayerMigrationData::Serialize(ByteBuffer& buffer) const
{
    ByteBuffer contentBuffer;
    
    // Write version
    contentBuffer << uint16(CURRENT_VERSION);
    
    // Core data
    contentBuffer << guid;
    contentBuffer << name;
    contentBuffer << accountId;
    contentBuffer << positionX << positionY << positionZ << orientation;
    contentBuffer << mapId << zoneId << areaId;
    contentBuffer << health << mana << level << race << class_ << gender;
    contentBuffer << xp << money << totalHonor << arenaPoints << totalKills;
    contentBuffer << maxHealth << maxMana << equipmentSetId;
    
    // Equipment
    for (const auto& item : equipment) {
        contentBuffer << item.exists;
        if (item.exists) {
            contentBuffer << item.entry << item.count << item.enchantId;
        }
    }
    
    // Spells and auras
    contentBuffer << static_cast<uint32>(spells.size());
    for (uint32 spellId : spells) {
        contentBuffer << spellId;
    }
    
    contentBuffer << static_cast<uint32>(auras.size());
    for (const auto& aura : auras) {
        contentBuffer << aura.spellId << aura.stacks << aura.duration;
    }
    
    // Quests
    contentBuffer << static_cast<uint32>(quests.size());
    for (const auto& quest : quests) {
        contentBuffer << quest.questId;
        contentBuffer << quest.status;
        contentBuffer << uint8(quest.rewarded ? 1 : 0);
        
        contentBuffer << static_cast<uint32>(quest.mobCounters.size());
        for (const auto& counter : quest.mobCounters) {
            contentBuffer << counter.first; // entry
            contentBuffer << counter.second; // count
        }
    }
    
    // Reputation
    contentBuffer << static_cast<uint32>(reputations.size());
    for (const auto& rep : reputations) {
        contentBuffer << rep.factionId;
        contentBuffer << rep.standing;
        contentBuffer << rep.flags;
    }
    
    // Action bars
    for (uint32 action : actionBars) {
        contentBuffer << action;
    }
    
    // Skills
    contentBuffer << static_cast<uint32>(skills.size());
    for (const auto& skill : skills) {
        contentBuffer << skill.first; // skillId
        contentBuffer << skill.second; // value
    }
    
    // Movement info
    contentBuffer << movementFlags;
    contentBuffer << transportGuid;
    contentBuffer << transportPosX << transportPosY << transportPosZ << transportPosO;
    contentBuffer << fallTime;
    
    // Compute checksum
    uint8 checksum[SHA_DIGEST_LENGTH];
    ComputeChecksum(contentBuffer.contents(), contentBuffer.size(), checksum);
    
    // Append to final buffer
    buffer.append(contentBuffer);
    buffer.append(checksum, SHA_DIGEST_LENGTH);
}

bool PlayerMigrationData::Deserialize(ByteBuffer& buffer)
{
    const size_t totalSize = buffer.size();
    const size_t minSize = sizeof(uint16) + SHA_DIGEST_LENGTH;
    
    if (totalSize < minSize) {
        sLog.outError("Migration data too small: %zu bytes", totalSize);
        return false;
    }
    
    // Extract data and checksum
    const size_t dataSize = totalSize - SHA_DIGEST_LENGTH;
    const uint8* dataPart = buffer.contents();
    const uint8* receivedChecksum = dataPart + dataSize;
    
    // Verify checksum
    if (!VerifyChecksum(buffer, dataSize)) {
        sLog.outError("Migration checksum mismatch");
        return false;
    }
    
    // Create view of data part
    ByteBuffer dataBuffer(dataPart, dataSize);
    
    try {
        // Read version
        uint16 version;
        dataBuffer >> version;
        
        if (version != CURRENT_VERSION) {
            sLog.outError("Unsupported migration version: %u (expected %u)", 
                         version, CURRENT_VERSION);
            return false;
        }
        
        // Deserialize core data
        dataBuffer >> guid;
        dataBuffer >> name;
        dataBuffer >> accountId;
        dataBuffer >> positionX >> positionY >> positionZ >> orientation;
        dataBuffer >> mapId >> zoneId >> areaId;
        dataBuffer >> health >> mana >> level >> race >> class_ >> gender;
        dataBuffer >> xp >> money >> totalHonor >> arenaPoints >> totalKills;
        dataBuffer >> maxHealth >> maxMana >> equipmentSetId;
        
        // Equipment
        for (auto& item : equipment) {
            dataBuffer >> item.exists;
            if (item.exists) {
                dataBuffer >> item.entry >> item.count >> item.enchantId;
            }
        }
        
        // Spells
        uint32 spellCount;
        dataBuffer >> spellCount;
        spells.resize(spellCount);
        for (uint32 i = 0; i < spellCount; ++i) {
            dataBuffer >> spells[i];
        }
        
        // Auras
        uint32 auraCount;
        dataBuffer >> auraCount;
        auras.resize(auraCount);
        for (uint32 i = 0; i < auraCount; ++i) {
            dataBuffer >> auras[i].spellId >> auras[i].stacks >> auras[i].duration;
        }
        
        // Quests
        uint32 questCount;
        dataBuffer >> questCount;
        quests.resize(questCount);
        for (auto& quest : quests) {
            dataBuffer >> quest.questId;
            dataBuffer >> quest.status;
            uint8 rewarded;
            dataBuffer >> rewarded;
            quest.rewarded = (rewarded == 1);
            
            uint32 counterCount;
            dataBuffer >> counterCount;
            quest.mobCounters.resize(counterCount);
            for (auto& counter : quest.mobCounters) {
                dataBuffer >> counter.first; // entry
                dataBuffer >> counter.second; // count
            }
        }
        
        // Reputation
        uint32 repCount;
        dataBuffer >> repCount;
        reputations.resize(repCount);
        for (auto& rep : reputations) {
            dataBuffer >> rep.factionId;
            dataBuffer >> rep.standing;
            dataBuffer >> rep.flags;
        }
        
        // Action bars
        for (uint32 i = 0; i < actionBars.size(); i++) {
            dataBuffer >> actionBars[i];
        }
        
        // Skills
        uint32 skillCount;
        dataBuffer >> skillCount;
        skills.resize(skillCount);
        for (auto& skill : skills) {
            dataBuffer >> skill.first; // skillId
            dataBuffer >> skill.second; // value
        }
        
        // Movement info
        dataBuffer >> movementFlags;
        dataBuffer >> transportGuid;
        dataBuffer >> transportPosX >> transportPosY >> transportPosZ >> transportPosO;
        dataBuffer >> fallTime;
        
        return true;
    }
    catch (const ByteBufferException& e) {
        sLog.outError("Deserialization error: %s", e.what());
        return false;
    }
    catch (const std::exception& e) {
        sLog.outError("Deserialization error: %s", e.what());
        return false;
    }
    catch (...) {
        sLog.outError("Deserialization error: Unknown exception");
        return false;
    }
}

void PlayerMigrationData::ComputeChecksum(const uint8* data, size_t len, uint8* out) const
{
    Sha1Hash sha;
    sha.UpdateData(data, len);
    sha.Finalize();
    memcpy(out, sha.GetDigest(), SHA_DIGEST_LENGTH);
}

bool PlayerMigrationData::VerifyChecksum(ByteBuffer& buffer, size_t dataSize) const
{
    const uint8* dataPart = buffer.contents();
    const uint8* receivedChecksum = dataPart + dataSize;
    
    uint8 computedChecksum[SHA_DIGEST_LENGTH];
    ComputeChecksum(dataPart, dataSize, computedChecksum);
    
    return memcmp(computedChecksum, receivedChecksum, SHA_DIGEST_LENGTH) == 0;
}