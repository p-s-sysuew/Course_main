#include "dbms.h"
#include "utils.h"
#include "tablelocks.h"
#include "securefile.h"
#include "course_storage.pb.h"

#include <stdexcept>
#include <mutex>
#include <iostream>

// Существует ли база
Database::Database(const std::filesystem::path& path)
    : path_(path)
{
    if (!std::filesystem::exists(path_))
    {
        throw std::runtime_error("база данных не существует: " + path_.filename().string());
    }
}

// Создание таблицы
void Database::createTable(const std::string& tableName, const std::vector<ColumnInfo>& columns) const
{
    Table::create(path_, tableName, columns);
}

// Удаление таблицы
void Database::dropTable(const TableName& table) const
{
    Table::drop(path_, table.tableName);
}

// Открытие таблицы
Table Database::openTable(const TableName& table) const
{
    return Table(path_, table.tableName);
}

// Создание субд
DBMS::DBMS(const std::filesystem::path& rootPath)
    : rootPath_(rootPath)
{
    ensureDirectoryExists(rootPath_);
}

void DBMS::setLogPath(const std::filesystem::path& logPath)
{
    logPath_ = logPath;
}

// Создание пути до бд
std::filesystem::path DBMS::databasePath(const std::string& databaseName) const
{
    return rootPath_ / databaseName;
}

// Определение имени бд
std::string DBMS::resolveDatabaseName(const TableName& table) const
{
    if (table.databaseName.has_value())
    {
        return table.databaseName.value();
    }

    if (currentDatabase_.empty())
    {
        throw std::runtime_error("активная база данных не выбрана, используйте USE database_name");
    }

    return currentDatabase_;
}

// Существование необходимой бд
Database DBMS::requireDatabaseFromTableName(const TableName& table) const
{
    return Database(databasePath(resolveDatabaseName(table)));
}


// Ключ таблицы, по которой создаётся мьютекс
static std::string makeTableLockKey(const std::string& databaseName, const std::string& tableName)
{
    return databaseName + "." + tableName;
}

// Определение нужного execute
std::string DBMS::execute(const Statement& statement)
{
    if (std::holds_alternative<CreateDatabaseCommand>(statement)) return executeCreateDatabase(std::get<CreateDatabaseCommand>(statement));
    if (std::holds_alternative<DropDatabaseCommand>(statement)) return executeDropDatabase(std::get<DropDatabaseCommand>(statement));
    if (std::holds_alternative<UseDatabaseCommand>(statement)) return executeUseDatabase(std::get<UseDatabaseCommand>(statement));
    if (std::holds_alternative<CreateTableCommand>(statement)) return executeCreateTable(std::get<CreateTableCommand>(statement));
    if (std::holds_alternative<DropTableCommand>(statement)) return executeDropTable(std::get<DropTableCommand>(statement));
    if (std::holds_alternative<InsertCommand>(statement)) return executeInsert(std::get<InsertCommand>(statement));
    if (std::holds_alternative<UpdateCommand>(statement)) return executeUpdate(std::get<UpdateCommand>(statement));
    if (std::holds_alternative<DeleteCommand>(statement)) return executeDelete(std::get<DeleteCommand>(statement));
    if (std::holds_alternative<SelectCommand>(statement)) return executeSelect(std::get<SelectCommand>(statement));
    if (std::holds_alternative<RevertCommand>(statement)) return executeRevert(std::get<RevertCommand>(statement));
    throw std::runtime_error("неизвестный тип команды");
}

// Создание бд
std::string DBMS::executeCreateDatabase(const CreateDatabaseCommand& command)
{
    std::lock_guard<std::mutex> lock(metadataMutex());
    if (!isValidIdentifier(command.databaseName))
    {
        throw std::runtime_error("некорректное имя базы данных: " + command.databaseName);
    }

    std::filesystem::path path = databasePath(command.databaseName);
    if (std::filesystem::exists(path))
    {
        throw std::runtime_error("база данных уже существует: " + command.databaseName);
    }

    std::filesystem::create_directories(path);
    return "OK: база данных создана: " + command.databaseName;
}

// Удаление бд
std::string DBMS::executeDropDatabase(const DropDatabaseCommand& command)
{
    std::lock_guard<std::mutex> lock(metadataMutex());
    std::filesystem::path path = databasePath(command.databaseName);
    if (!std::filesystem::exists(path))
    {
        throw std::runtime_error("база данных не существует: " + command.databaseName);
    }

    std::filesystem::remove_all(path);
    if (currentDatabase_ == command.databaseName)
    {
        currentDatabase_.clear();
    }

    return "OK: база данных удалена: " + command.databaseName;
}

// Выбор бд
std::string DBMS::executeUseDatabase(const UseDatabaseCommand& command)
{
    std::lock_guard<std::mutex> lock(metadataMutex());
    if (!std::filesystem::exists(databasePath(command.databaseName)))
    {
        throw std::runtime_error("база данных не существует: " + command.databaseName);
    }

    currentDatabase_ = command.databaseName;
    return "OK: выбрана база данных: " + currentDatabase_;
}

// Создание таблицы в бд
std::string DBMS::executeCreateTable(const CreateTableCommand& command)
{
    if (currentDatabase_.empty())
    {
        throw std::runtime_error("CREATE TABLE требует выбранную базу данных, используйте USE");
    }

    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(currentDatabase_, command.tableName)));
    Database database(databasePath(currentDatabase_));
    database.createTable(command.tableName, command.columns);
    return "OK: таблица создана: " + command.tableName;
}

// Удаление таблицы в бд
std::string DBMS::executeDropTable(const DropTableCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table);
    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(databaseName, command.table.tableName)));
    Database database(databasePath(databaseName));
    database.dropTable(command.table);
    return "OK: таблица удалена: " + command.table.tableName;
}

// Добавление строк в таблицу в бд
std::string DBMS::executeInsert(const InsertCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table);
    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(databaseName, command.table.tableName)));
    Database database(databasePath(databaseName));
    Table table = database.openTable(command.table);
    std::size_t count = table.insertRows(command.columns, command.rows);
    return "OK: вставлено строк: " + std::to_string(count);
}

// Изменение строк в таблице в бд
std::string DBMS::executeUpdate(const UpdateCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table);
    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(databaseName, command.table.tableName)));
    Database database(databasePath(databaseName));
    Table table = database.openTable(command.table);
    std::size_t count = table.updateRows(command.assignments, command.where);
    return "OK: обновлено строк: " + std::to_string(count);
}

// Удаление строк в таблице в бд
std::string DBMS::executeDelete(const DeleteCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table);
    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(databaseName, command.table.tableName)));
    Database database(databasePath(databaseName));
    Table table = database.openTable(command.table);
    std::size_t count = table.deleteRows(command.where);
    return "OK: удалено строк: " + std::to_string(count);
}

// Чтение строк в таблице в бд
std::string DBMS::executeSelect(const SelectCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table);
    Database database(databasePath(databaseName));
    Table table = database.openTable(command.table);
    SelectResult result = table.selectRows(command.selectAll, command.items, command.where);
    return result.json;
}

std::string DBMS::executeRevert(const RevertCommand& command)
{
    if (logPath_.empty())
    {
        throw std::runtime_error("REVERT: путь к логам не задан");
    }

    if (!std::filesystem::exists(logPath_))
    {
        throw std::runtime_error("REVERT: файл логов не найден");
    }

    std::vector<std::string> records = readSecureRecords(logPath_);
    std::vector<std::string> commandsToReplay;

    for (const auto& record : records)
    {
        ProtoLogEntry entry;
        if (!entry.ParseFromString(record)) continue;
        if (entry.status() != "OK") continue;
        if (entry.start_time() > command.timestamp) break;

        std::string sql = entry.query_text();
        if (toUpper(trim(sql)).find("REVERT") == 0) continue;

        commandsToReplay.push_back(sql);
    }

    // Full system revert: wipe everything but auth/system data
    for (const auto& entry : std::filesystem::directory_iterator(rootPath_))
    {
        if (entry.path().filename() != "_system" && entry.path().filename() != "users.pb")
        {
            std::filesystem::remove_all(entry.path());
        }
    }

    Parser parser;
    currentDatabase_.clear();

    for (const auto& sql : commandsToReplay)
    {
        try {
            std::vector<std::string> statements = splitStatements(sql);
            for (const auto& stmtText : statements) {
                Statement stmt = parser.parseStatement(stmtText);
                execute(stmt);
            }
        } catch (...) {
            // Replay as much as possible
        }
    }

    return "OK: состояние базы данных откачено к " + command.timestamp;
}
