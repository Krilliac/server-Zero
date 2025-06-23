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

#include "Config.h"
#include "Log.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

Config& Config::instance()
{
    static Config instance;
    return instance;
}

bool Config::Load(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configFile = filename;
    ParseFile(filename);
    return true;
}

bool Config::Reload()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    if (!m_configFile.empty()) {
        ParseFile(m_configFile);
        NotifyReload();
        return true;
    }
    return false;
}

void Config::ParseFile(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        sLog.outError("Config: Could not open %s", filename.c_str());
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Remove comments
        size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos)
            line = line.substr(0, comment);

        // Trim whitespace
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](int ch) {
            return !std::isspace(ch);
        }));
        line.erase(std::find_if(line.rbegin(), line.rend(), [](int ch) {
            return !std::isspace(ch);
        }).base(), line.end());

        if (line.empty())
            continue;

        // Split key and value
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim whitespace from key and value
        key.erase(key.begin(), std::find_if(key.begin(), key.end(), [](int ch) {
            return !std::isspace(ch);
        }));
        key.erase(std::find_if(key.rbegin(), key.rend(), [](int ch) {
            return !std::isspace(ch);
        }).base(), key.end());

        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](int ch) {
            return !std::isspace(ch);
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [](int ch) {
            return !std::isspace(ch);
        }).base(), value.end());

        if (!key.empty())
            m_entries[key] = value;
    }
}

std::string Config::GetStringDefault(const std::string& name, const std::string& defaultValue) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(name);
    return it != m_entries.end() ? it->second : defaultValue;
}

int Config::GetIntDefault(const std::string& name, int defaultValue) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(name);
    if (it == m_entries.end())
        return defaultValue;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return defaultValue;
    }
}

float Config::GetFloatDefault(const std::string& name, float defaultValue) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(name);
    if (it == m_entries.end())
        return defaultValue;
    try {
        return std::stof(it->second);
    } catch (...) {
        return defaultValue;
    }
}

bool Config::GetBoolDefault(const std::string& name, bool defaultValue) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(name);
    if (it == m_entries.end())
        return defaultValue;
    std::string value = it->second;
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

void Config::RegisterReloadCallback(const std::function<void()>& callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_reloadCallbacks.push_back(callback);
}

void Config::NotifyReload()
{
    for (auto& callback : m_reloadCallbacks) {
        callback();
    }
}

