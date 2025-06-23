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

#include "DatabaseEnv.h"
#include "Database/SqlOperations.h"
#include "Database/SqlDelayThread.h"
#include "Database/SqlTransaction.h"
#include "Policies/Singleton.h"
#include "Log.h"
#include "Config/Config.h"
#include "GitRevision.h"

#include <ctime>
#include <iostream>
#include <fstream>
#include <memory>
#include <cstdarg>
#include <algorithm>

#define MIN_CONNECTION_POOL_SIZE 1
#define MAX_CONNECTION_POOL_SIZE 16

const DBVersion Database::databaseVersions[COUNT_DATABASES] = {
    { "World", GitRevision::GetWorldDBVersion(), GitRevision::GetWorldDBStructure(), GitRevision::GetWorldDBContent(), GitRevision::GetWorldDBUpdateDescription() },
    { "Character", GitRevision::GetCharDBVersion(), GitRevision::GetCharDBStructure(), GitRevision::GetCharDBContent(), GitRevision::GetCharDBUpdateDescription() },
    { "Realmd", GitRevision::GetRealmDBVersion(), GitRevision::GetRealmDBStructure(), GitRevision::GetRealmDBContent(), GitRevision::GetRealmDBUpdateDescription() }
};

Database::Database() :
    m_pResultQueue(NULL),
    m_pAsyncConn(NULL),
    m_threadBody(NULL),
    m_delayThread(NULL),
    m_logSQL(false),
    m_bAllowAsyncTransactions(true),
    m_iStmtIndex(0),
    m_TransStorage(NULL),
    m_nQueryConnPoolSize(0),
    m_nQueryCounter(0),
    m_pingIntervallms(0)
{
}

Database::~Database()
{
    StopServer();
}

bool Database::Initialize(const char* infoString, int nConns)
{
    // Enable logging of SQL commands
    m_logSQL = sConfig.GetBoolDefault("LogSQL", false);
    m_logsDir = sConfig.GetStringDefault("LogsDir", "");
    if (!m_logsDir.empty())
    {
        if ((m_logsDir.at(m_logsDir.length() - 1) != '/') && (m_logsDir.at(m_logsDir.length() - 1) != '\\'))
            m_logsDir.append("/");
    }
    
    m_pingIntervallms = sConfig.GetIntDefault("MaxPingTime", 30) * (MINUTE * 1000);

    // Setup connection pool size
    if (nConns < MIN_CONNECTION_POOL_SIZE)
        m_nQueryConnPoolSize = MIN_CONNECTION_POOL_SIZE;
    else if (nConns > MAX_CONNECTION_POOL_SIZE)
        m_nQueryConnPoolSize = MAX_CONNECTION_POOL_SIZE;
    else
        m_nQueryConnPoolSize = nConns;

    // Create connection pool for sync requests
    for (int i = 0; i < m_nQueryConnPoolSize; ++i)
    {
        SqlConnection* pConn = CreateConnection();
        if (!pConn->Initialize(infoString))
        {
            delete pConn;
            return false;
        }
        m_pQueryConnections.push_back(pConn);
    }

    // Create and initialize connection for async requests
    m_pAsyncConn = CreateConnection();
    if (!m_pAsyncConn->Initialize(infoString))
        return false;

    m_pResultQueue = new SqlResultQueue;

    InitDelayThread();
    return true;
}

void Database::StopServer()
{
    HaltDelayThread();

    delete m_pResultQueue;
    delete m_pAsyncConn;

    m_pResultQueue = NULL;
    m_pAsyncConn = NULL;

    for (size_t i = 0; i < m_pQueryConnections.size(); ++i)
        delete m_pQueryConnections[i];

    m_pQueryConnections.clear();
}

SqlDelayThread* Database::CreateDelayThread()
{
    assert(m_pAsyncConn);
    return new SqlDelayThread(this, m_pAsyncConn);
}

void Database::InitDelayThread()
{
    assert(!m_delayThread);

    m_threadBody = CreateDelayThread();
    m_TransStorage = new ACE_TSS<TransHelper>();
    m_delayThread = new ACE_Based::Thread(m_threadBody);
}

void Database::HaltDelayThread()
{
    if (!m_threadBody || !m_delayThread) return;

    m_threadBody->Stop();
    m_delayThread->wait();
    delete m_TransStorage;
    delete m_delayThread;
    m_delayThread = NULL;
    m_threadBody = NULL;
    m_TransStorage = NULL;
}

void Database::ThreadStart()
{
}

void Database::ThreadEnd()
{
}

void Database::ProcessResultQueue()
{
    if (m_pResultQueue)
        m_pResultQueue->Update();
}

void Database::EscapeString(std::string& str)
{
    if (str.empty()) return;

    char* buf = new char[str.size() * 2 + 1];
    m_pQueryConnections[0]->escape_string(buf, str.c_str(), str.size());
    str = buf;
    delete[] buf;
}

SqlConnection* Database::getQueryConnection()
{
    int nCount = 0;
    if (m_nQueryCounter == long(1 << 31))
        m_nQueryCounter = 0;
    else
        nCount = ++m_nQueryCounter;

    return m_pQueryConnections[nCount % m_nQueryConnPoolSize];
}

void Database::Ping()
{
    const char* sql = "SELECT 1";
    
    {
        SqlConnection::Lock guard(m_pAsyncConn);
        delete guard->Query(sql);
    }

    for (int i = 0; i < m_nQueryConnPoolSize; ++i)
    {
        SqlConnection::Lock guard(m_pQueryConnections[i]);
        delete guard->Query(sql);
    }
}

bool Database::PExecuteLog(const char* format, ...)
{
    if (!format) return false;

    va_list ap;
    char szQuery [MAX_QUERY_LEN];
    va_start(ap, format);
    int res = vsnprintf(szQuery, MAX_QUERY_LEN, format, ap);
    va_end(ap);

    if (res == -1)
    {
        sLog.outError("SQL Query truncated (and not execute) for format: %s", format);
        return false;
    }

    if (m_logSQL)
    {
        time_t curr;
        tm local;
        time(&curr);
        local = *(localtime(&curr));
        char fName[128];
        sprintf(fName, "%04d-%02d-%02d_logSQL.sql", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);

        FILE* log_file;
        std::string logsDir_fname = m_logsDir + fName;
        log_file = fopen(logsDir_fname.c_str(), "a");
        if (log_file)
        {
            fprintf(log_file, "%s;\n", szQuery);
            fclose(log_file);
        }
        else
            sLog.outError("SQL-Logging disabled - Can't open log file: %s", fName);
    }

    return Execute(szQuery);
}

QueryResult* Database::PQuery(const char* format, ...)
{
    if (!format) return NULL;

    va_list ap;
    char szQuery [MAX_QUERY_LEN];
    va_start(ap, format);
    int res = vsnprintf(szQuery, MAX_QUERY_LEN, format, ap);
    va_end(ap);

    if (res == -1)
    {
        sLog.outError("SQL Query truncated (and not execute) for format: %s", format);
        return NULL;
    }

    return Query(szQuery);
}

QueryNamedResult* Database::PQueryNamed(const char* format, ...)
{
    if (!format) return NULL;

    va_list ap;
    char szQuery [MAX_QUERY_LEN];
    va_start(ap, format);
    int res = vsnprintf(szQuery, MAX_QUERY_LEN, format, ap);
    va_end(ap);

    if (res == -1)
    {
        sLog.outError("SQL Query truncated (and not execute) for format: %s", format);
        return NULL;
    }

    return QueryNamed(szQuery);
}

bool Database::Execute(const char* sql)
{
    if (!m_pAsyncConn) return false;

    SqlTransaction* pTrans = (*m_TransStorage)->get();
    if (pTrans)
        pTrans->DelayExecute(new SqlPlainRequest(sql));
    else
    {
        if (!m_bAllowAsyncTransactions)
            return DirectExecute(sql);

        m_threadBody->Delay(new SqlPlainRequest(sql));
    }

    return true;
}

bool Database::PExecute(const char* format, ...)
{
    if (!format) return false;

    va_list ap;
    char szQuery [MAX_QUERY_LEN];
    va_start(ap, format);
    int res = vsnprintf(szQuery, MAX_QUERY_LEN, format, ap);
    va_end(ap);

    if (res == -1)
    {
        sLog.outError("SQL Query truncated (and not execute) for format: %s", format);
        return false;
    }

    return Execute(szQuery);
}

bool Database::DirectPExecute(const char* format, ...)
{
    if (!format) return false;

    va_list ap;
    char szQuery [MAX_QUERY_LEN];
    va_start(ap, format);
    int res = vsnprintf(szQuery, MAX_QUERY_LEN, format, ap);
    va_end(ap);

    if (res == -1)
    {
        sLog.outError("SQL Query truncated (and not execute) for format: %s", format);
        return false;
    }

    return DirectExecute(szQuery);
}

bool Database::BeginTransaction()
{
    if (!m_pAsyncConn) return false;
    (*m_TransStorage)->init();
    return true;
}

bool Database::CommitTransaction()
{
    if (!m_pAsyncConn) return false;
    if (!(*m_TransStorage)->get()) return false;

    if (!m_bAllowAsyncTransactions)
        return CommitTransactionDirect();

    m_threadBody->Delay((*m_TransStorage)->detach());
    return true;
}

bool Database::CommitTransactionDirect()
{
    if (!m_pAsyncConn) return false;
    if (!(*m_TransStorage)->get()) return false;

    SqlTransaction* pTrans = (*m_TransStorage)->detach();
    pTrans->Execute(m_pAsyncConn);
    delete pTrans;
    return true;
}

bool Database::RollbackTransaction()
{
    if (!m_pAsyncConn) return false;
    if (!(*m_TransStorage)->get()) return false;
    (*m_TransStorage)->reset();
    return true;
}

bool Database::CheckDatabaseVersion(DatabaseType database)
{
    const DBVersion& core_db_requirements = databaseVersions[database];
    QueryResult* result = Query("SELECT `version`, `structure`, `content`, `description` FROM `db_version` ORDER BY `version` DESC, `structure` DESC, `content` DESC LIMIT 1");

    if (!result)
    {
        sLog.outErrorDb("Table `db_version` missing or corrupt in [%s] database", core_db_requirements.dbname.c_str());
        return false;
    }

    Field* fields = result->Fetch();
    std::string current_db_version = fields[0].GetCppString();
    std::string current_db_structure = fields[1].GetCppString();
    std::string current_db_content = fields[2].GetCppString();
    std::string description = fields[3].GetCppString();
    delete result;

    // Structure validation
    if (current_db_version != core_db_requirements.expected_version || 
        current_db_structure != core_db_requirements.expected_structure)
    {
        sLog.outErrorDb("Database structure mismatch for [%s]", core_db_requirements.dbname.c_str());
        sLog.outErrorDb("Current: Version=%s, Structure=%s", 
            current_db_version.c_str(), current_db_structure.c_str());
        sLog.outErrorDb("Required: Version=%s, Structure=%s",
            core_db_requirements.expected_version.c_str(),
            core_db_requirements.expected_structure.c_str());
        return false;
    }

    // Content validation
    if (current_db_content < core_db_requirements.minimal_expected_content)
    {
        sLog.outErrorDb("Database content too old for [%s]", core_db_requirements.dbname.c_str());
        sLog.outErrorDb("Current: %s, Required: %s", 
            current_db_content.c_str(),
            core_db_requirements.minimal_expected_content.c_str());
        return false;
    }

    sLog.outString("Validated %s database: Version=%s, Structure=%s, Content=%s",
        core_db_requirements.dbname.c_str(),
        current_db_version.c_str(),
        current_db_structure.c_str(),
        current_db_content.c_str());
    return true;
}

bool Database::ExecuteStmt(const SqlStatementID& id, SqlStmtParameters* params)
{
    if (!m_pAsyncConn) return false;

    SqlTransaction* pTrans = (*m_TransStorage)->get();
    if (pTrans)
        pTrans->DelayExecute(new SqlPreparedRequest(id.ID(), params));
    else
    {
        if (!m_bAllowAsyncTransactions)
            return DirectExecuteStmt(id, params);

        m_threadBody->Delay(new SqlPreparedRequest(id.ID(), params));
    }

    return true;
}

bool Database::DirectExecuteStmt(const SqlStatementID& id, SqlStmtParameters* params)
{
    MANGOS_ASSERT(params);
    std::shared_ptr<SqlStmtParameters> p(params);
    SqlConnection::Lock _guard(getAsyncConnection());
    return _guard->ExecuteStmt(id.ID(), *params);
}

SqlStatement Database::CreateStatement(SqlStatementID& index, const char* fmt)
{
    int nId = -1;
    if (!index.initialized())
    {
        std::string szFmt(fmt);
        int nParams = std::count(szFmt.begin(), szFmt.end(), '?');
        ACE_Guard<ACE_Thread_Mutex> _guard(m_stmtGuard);
        PreparedStmtRegistry::const_iterator iter = m_stmtRegistry.find(szFmt);
        if (iter == m_stmtRegistry.end())
        {
            nId = ++m_iStmtIndex;
            m_stmtRegistry[szFmt] = nId;
        }
        else
            nId = iter->second;

        index.init(nId, nParams);
    }

    return SqlStatement(index, *this);
}

std::string Database::GetStmtString(const int stmtId) const
{
    if (stmtId == -1 || stmtId > m_iStmtIndex)
        return std::string();

    ACE_Guard<ACE_Thread_Mutex> _guard(m_stmtGuard);
    for (PreparedStmtRegistry::const_iterator iter = m_stmtRegistry.begin(); iter != m_stmtRegistry.end(); ++iter)
        if (iter->second == stmtId)
            return iter->first;

    return std::string();
}