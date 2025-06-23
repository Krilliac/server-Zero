#include "CacheMgr.h"

// Global cache instantiations
std::shared_ptr<LRUCache<uint32_t, SpellEntry>> sSpellCache = 
    sCacheMgr->CreateCache<uint32_t, SpellEntry>(
        "SpellCache",
        5000,
        [](uint32_t spellId) {
            return sSpellStore.LookupEntry(spellId);
        });

std::shared_ptr<LRUCache<ObjectGuid, PlayerInfo>> sPlayerInfoCache = 
    sCacheMgr->CreateCache<ObjectGuid, PlayerInfo>(
        "PlayerInfoCache",
        10000,
        [](ObjectGuid guid) {
            return sCharacterDatabase.LoadPlayerInfo(guid);
        });

std::shared_ptr<LRUCache<uint32_t, ItemPrototype>> sItemTemplateCache = 
    sCacheMgr->CreateCache<uint32_t, ItemPrototype>(
        "ItemTemplateCache",
        20000,
        [](uint32_t itemId) {
            return sItemStorage.LookupEntry(itemId);
        });
