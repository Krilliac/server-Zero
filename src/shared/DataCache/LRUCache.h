#pragma once
#include <list>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <stdexcept>

template<typename Key, typename Value>
class LRUCache
{
public:
    using LoadFunction = std::function<Value(const Key&)>;
    
    LRUCache(size_t capacity, LoadFunction loader)
        : m_capacity(capacity), m_loader(loader)
    {
        if (capacity == 0) {
            throw std::invalid_argument("Cache capacity must be > 0");
        }
    }
    
    Value Get(const Key& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            Touch(it->second.second);
            return it->second.first;
        }
        
        Value value = m_loader(key);
        Put(key, value);
        return value;
    }
    
    void Put(const Key& key, const Value& value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            Touch(it->second.second);
            it->second.first = value;
            return;
        }
        
        if (m_cache.size() >= m_capacity) {
            auto last = m_accessOrder.end();
            --last;
            m_cache.erase(*last);
            m_accessOrder.pop_back();
        }
        
        m_accessOrder.push_front(key);
        m_cache[key] = {value, m_accessOrder.begin()};
    }
    
    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache.clear();
        m_accessOrder.clear();
    }
    
    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cache.size();
    }

private:
    void Touch(typename std::list<Key>::iterator it)
    {
        m_accessOrder.splice(m_accessOrder.begin(), m_accessOrder, it);
    }
    
    size_t m_capacity;
    LoadFunction m_loader;
    mutable std::mutex m_mutex;
    std::list<Key> m_accessOrder;
    std::unordered_map<Key, std::pair<Value, typename std::list<Key>::iterator>> m_cache;
};
