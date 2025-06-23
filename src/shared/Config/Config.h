/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2025 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */
 
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <functional>

class Config
{
public:
    static Config& instance();

    // Loads the configuration file (INI-like format)
    bool Load(const std::string& filename);

    // Reloads the last loaded file and notifies callbacks
    bool Reload();

    // Value retrieval with defaults
    std::string GetStringDefault(const std::string& name, const std::string& defaultValue) const;
    int GetIntDefault(const std::string& name, int defaultValue) const;
    float GetFloatDefault(const std::string& name, float defaultValue) const;
    bool GetBoolDefault(const std::string& name, bool defaultValue) const;

    // Hot-reload support
    void RegisterReloadCallback(const std::function<void()>& callback);

private:
    Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    void ParseFile(const std::string& filename);
    void NotifyReload();

    std::unordered_map<std::string, std::string> m_entries;
    std::vector<std::function<void()>> m_reloadCallbacks;
    mutable std::mutex m_mutex;
    std::string m_configFile;
};

#define sConfig (Config::instance())
