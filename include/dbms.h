#pragma once

#include "parser.h"
#include "table.h"

#include <filesystem> 
#include <string>

class Database
{
public:
    explicit Database(const std::filesystem::path& path);

    void createTable(const std::string& tableName, const std::vector<ColumnInfo>& columns) const;

    void dropTable(const TableName& table) const;

    Table openTable(const TableName& table) const;

private:
    std::filesystem::path path_;
};

class DBMS
{
public:
    explicit DBMS(const std::filesystem::path& rootPath);
    void setLogPath(const std::filesystem::path& logPath);

    std::string execute(const Statement& statement);

private:
    std::filesystem::path rootPath_;
    std::filesystem::path logPath_;
    std::string currentDatabase_;

    std::filesystem::path databasePath(const std::string& databaseName) const;
    std::string resolveDatabaseName(const TableName& table) const;
    Database requireDatabaseFromTableName(const TableName& table) const;
    std::string executeCreateDatabase(const CreateDatabaseCommand& command);
    std::string executeDropDatabase(const DropDatabaseCommand& command);
    std::string executeUseDatabase(const UseDatabaseCommand& command);
    std::string executeCreateTable(const CreateTableCommand& command);
    std::string executeDropTable(const DropTableCommand& command);
    std::string executeInsert(const InsertCommand& command);
    std::string executeUpdate(const UpdateCommand& command);
    std::string executeDelete(const DeleteCommand& command);
    std::string executeSelect(const SelectCommand& command);
    std::string executeRevert(const RevertCommand& command);
};
