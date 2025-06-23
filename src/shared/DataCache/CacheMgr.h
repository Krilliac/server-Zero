#pragma once
#include "LRUCache.h"
#include "Log.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <any>
#include <stdexcept>

class CacheMgr
{
public:
    static CacheMgr* instance()
    {
        static CacheMgr instance;
        return &instance;
    }
    
    template<typename Key, typename Value>
    std::shared_ptr<LRUCache<Key, Value>> CreateCache(
        const std::string& name, 
        size_t capacity,
        typename LRUCache<Key, Value>::LoadFunction loader)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto cache = std::make_shared<LRUCache<Key, Value>>(capacity, loader);
        m_caches[name] = cache;
        sLog.outDebug("Created cache '%s' with capacity %zu", name.c_str(), capacity);
        return cache;
    }
    
    template<typename Key, typename Value>
    std::shared_ptr<LRUCache<Key, Value>> GetCache(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_caches.find(name);
        if (it == m_caches.end()) {
            sLog.outDebug("CacheMgr::GetCache: Cache '%s' not found", name.c_str());
            return nullptr;
        }
        
        try {
            return std::any_cast<std::shared_ptr<LRUCache<Key, Value>>>(it->second);
        } 
        catch (const std::bad_any_cast& e) {
            sLog.outError("CacheMgr::GetCache: Type mismatch for cache '%s': %s", 
                         name.c_str(), e.what());
        }
        catch (const std::exception& e) {
            sLog.outError("CacheMgr::GetCache: Exception for cache '%s': %s", 
                         name.c_str(), e.what());
        }
        catch (...) {
            sLog.outError("CacheMgr::GetCache: Unknown exception for cache '%s'", 
                         name.c_str());
        }
        return nullptr;
    }
    
    void ClearCache(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_caches.find(name);
        if (it == m_caches.end()) {
            sLog.outDebug("CacheMgr::ClearCache: Cache '%s' not found", name.c_str());
            return;
        }
        
        try {
            // Attempt to clear common cache types
            if (auto cache = std::any_cast<std::shared_ptr<LRUCache<uint32_t, std::string>>>(&it->second)) {
                (*cache)->Clear();
                sLog.outDebug("Cleared cache '%s' (type: uint32->string)", name.c_str());
            } 
            else if (auto cache = std::any_cast<std::shared_ptr<LRUCache<ObjectGuid, PlayerInfo>>>(&it->second)) {
                (*cache)->Clear();
                sLog.outDebug("Cleared cache '%s' (type: ObjectGuid->PlayerInfo)", name.c_str());
            } 
            else if (auto cache = std::any_cast<std::shared_ptr<LRUCache<uint32_t, ItemPrototype>>>(&it->second)) {
                (*cache)->Clear();
                sLog.outDebug("Cleared cache '%s' (type: uint32->ItemPrototype)", name.c_str());
            } 
            else {
                sLog.outError("CacheMgr::ClearCache: Unknown cache type for '%s'", name.c_str());
            }
        } 
        catch (const std::bad_any_cast& e) {
            sLog.outError("CacheMgr::ClearCache: Type mismatch for cache '%s': %s", 
                         name.c_str(), e.what());
        }
        catch (const std::exception& e) {
            sLog.outError("CacheMgr::ClearCache: Standard exception for cache '%s': %s", 
                         name.c_str(), e.what());
        }
        catch (...) {
            sLog.outError("CacheMgr::ClearCache: Non-standard exception for cache '%s'", 
                         name.c_str());
        }
    }
    
    void ClearAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sLog.outDebug("Clearing all caches (%zu total)", m_caches.size());
        
        for (auto& [name, cache] : m_caches) {
            try {
                // Attempt to clear common cache types
                if (auto ptr = std::any_cast<std::shared_ptr<LRUCache<uint32_t, std::string>>>(&cache)) {
                    (*ptr)->Clear();
                } 
                else if (auto ptr = std::any_cast<std::shared_ptr<LRUCache<ObjectGuid, PlayerInfo>>>(&cache)) {
                    (*ptr)->Clear();
                } 
                else if (auto ptr = std::any_cast<std::shared_ptr<LRUCache<uint32_t, ItemPrototype>>>(&cache)) {
                    (*ptr)->Clear();
                } 
                else {
                    sLog.outError("CacheMgr::ClearAll: Unknown cache type for '%s'", name.c_str());
                }
            } 
            catch (const std::bad_any_cast& e) {
                sLog.outError("CacheMgr::ClearAll: Type mismatch for cache '%s': %s", 
                             name.c_str(), e.what());
            }
            catch (const std::exception& e) {
                sLog.outError("CacheMgr::ClearAll: Standard exception for cache '%s': %s", 
                             name.c_str(), e.what());
            }
            catch (...) {
                sLog.outError("CacheMgr::ClearAll: Non-standard exception for cache '%s'", 
                             name.c_str());
            }
        }
    }

    size_t GetCacheCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_caches.size();
    }

private:
    std::unordered_map<std::string, std::any> m_caches;
    mutable std::mutex m_mutex;
};

#define sCacheMgr CacheMgr::instance()
