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

#ifndef DATABASE_ENV_H
#define DATABASE_ENV_H

#include "Config.h"
#include "Log.h"
#include "GitRevision.h"
#include "Policies/Singleton.h"
#include "Database/SqlOperations.h"
#include "Database/SqlDelayThread.h"
#include "Database/SqlTransaction.h"
#include "Database/Field.h"
#include "Database/QueryResult.h"

#include <mysql.h>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <queue>
#include <atomic>
#include <functional>
#include <stdexcept>

enum DatabaseType
{
    DATABASE_WORLD      = 0,
    DATABASE_CHARACTER  = 1,
    DATABASE_LOGIN      = 2,
    COUNT_DATABASES
};

struct DBVersion
{
    std::string dbname;
    std::string expected_version;
    std::string expected_structure;
    std::string minimal_expected_content;
    std::string description;
};

class Database
{
    public:
        virtual ~Database();

        static Database* Create(DatabaseType type);
        
        bool Initialize(const char* infoString, int nConns = 1);
        void StopServer();
        
        // Synchronous query execution
        QueryResult* Query(const char* sql);
        QueryNamedResult* QueryNamed(const char* sql);
        bool Execute(const char* sql);
        bool DirectExecute(const char* sql);
        bool PExecute(const char* format, ...);
        bool DirectPExecute(const char* format, ...);
        
        // Asynchronous query execution
        bool AsyncQuery(const char* sql, QueryCallback callback);
        bool AsyncPExecute(const char* format, ...);
        
        // Transactions
        bool BeginTransaction();
        bool CommitTransaction();
        bool CommitTransactionDirect();
        bool RollbackTransaction();
        
        // Prepared statements
        SqlStatement CreateStatement(SqlStatementID& index, const char* fmt);
        bool ExecuteStmt(const SqlStatementID& id, SqlStmtParameters* params);
        bool DirectExecuteStmt(const SqlStatementID& id, SqlStmtParameters* params);
        
        // Maintenance
        void Ping();
        void EscapeString(std::string& str);
        bool CheckDatabaseVersion(DatabaseType database);
        
        // Configuration
        void SetLogging(bool enable) { m_logSQL = enable; }
        void SetAllowAsyncTransactions(bool allow) { m_bAllowAsyncTransactions = allow; }
        
    protected:
        Database();
        
        virtual SqlDelayThread* CreateDelayThread() = 0;
        virtual SqlConnection* CreateConnection() = 0;
        
        bool OpenConnections(int innerLoop, const char* infoString);
        size_t db_count = 0;
        
    private:
        // Connection management
        SqlConnection* getQueryConnection();
        SqlConnection* getAsyncConnection() { return m_pAsyncConn; }
        
        // Prepared statement registry
        std::string GetStmtString(const int stmtId) const;
        
        // Thread management
        void InitDelayThread();
        void HaltDelayThread();
        
        // Version management
        static const DBVersion databaseVersions[COUNT_DATABASES];
        
        // Connection pools
        std::vector<SqlConnection*> m_pQueryConnections;
        SqlConnection* m_pAsyncConn;
        SqlResultQueue* m_pResultQueue;
        SqlDelayThread* m_threadBody;
        ACE_Based::Thread* m_delayThread;
        
        // Configuration
        bool m_logSQL;
        bool m_bAllowAsyncTransactions;
        std::string m_logsDir;
        
        // Prepared statements
        typedef UNORDERED_MAP<std::string, int> PreparedStmtRegistry;
        PreparedStmtRegistry m_stmtRegistry;
        mutable ACE_Thread_Mutex m_stmtGuard;
        int m_iStmtIndex;
        
        // Transaction support
        class TransHelper
        {
            public:
                TransHelper() : m_pTrans(NULL) {}
                ~TransHelper() { reset(); }
                
                SqlTransaction* init()
                {
                    MANGOS_ASSERT(!m_pTrans);
                    m_pTrans = new SqlTransaction;
                    return m_pTrans;
                }
                
                SqlTransaction* get() { return m_pTrans; }
                SqlTransaction* detach()
                {
                    SqlTransaction* pRes = m_pTrans;
                    m_pTrans = NULL;
                    return pRes;
                }
                
                void reset()
                {
                    delete m_pTrans;
                    m_pTrans = NULL;
                }
                
            private:
                SqlTransaction* m_pTrans;
        };
        
        ACE_TSS<TransHelper>* m_TransStorage;
        
        // Connection counters
        int m_nQueryConnPoolSize;
        long m_nQueryCounter;
        uint32 m_pingIntervallms;
};

#endif
