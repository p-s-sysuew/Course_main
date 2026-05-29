#include "dbms.h"
#include "utils.h"
#include "tablelocks.h"

#include <stdexcept>
#include <mutex>

// Конструктор Database: инициализирует путь к базе и проверяет ее физическое существование
Database::Database(const std::filesystem::path& path)
    : path_(path)
{
    // Если директория базы данных отсутствует на диске
    if (!std::filesystem::exists(path_))
    {
        throw std::runtime_error("База данных не была создана (не существует): " + path_.filename().string()); // Ошибка отсутствия БД
    }
}

// Создание новой таблицы через вызов статического метода класса Table
void Database::createTable(const std::string& tableName, const std::vector<ColumnInfo>& columns) const
{
    Table::create(path_, tableName, columns); // Делегирование создания структуры файлов таблицы
}

// Удаление файлов таблицы с диска
void Database::dropTable(const TableName& table) const
{
    Table::drop(path_, table.tableName); // Делегирование удаления файлов таблицы
}

// Открытие существующей таблицы для последующей работы с ней
Table Database::openTable(const TableName& table) const
{
    return Table(path_, table.tableName); // Возврат инициализированного объекта таблицы
}

// Конструктор СУБД: задает корневой каталог и создает его при необходимости
DBMS::DBMS(const std::filesystem::path& rootPath)
    : rootPath_(rootPath)
{
    ensureDirectoryExists(rootPath_); // Гарантированное создание корневой папки СУБД на диске
}

// Формирование полного пути к директории конкретной базы данных
std::filesystem::path DBMS::databasePath(const std::string& databaseName) const
{
    return rootPath_ / databaseName; // Конкатенация корневого пути и имени базы
}

// Определение целевой БД: из параметров запроса либо из текущего контекста USE
std::string DBMS::resolveDatabaseName(const TableName& table) const
{
    // Если имя базы явно указано в запросе
    if (table.databaseName.has_value())
    {
        return table.databaseName.value(); // Возврат явно переданного имени БД
    }

    // Если имя не указано и текущий контекст пуст
    if (currentDatabase_.empty())
    {
        throw std::runtime_error("активная база данных не выбрана, используйте USE database_name"); 
    }

    return currentDatabase_; // Возврат имени активной в данный момент БД
}

// Вспомогательный метод для получения объекта БД на основе структуры имени таблицы
Database DBMS::requireDatabaseFromTableName(const TableName& table) const
{
    return Database(databasePath(resolveDatabaseName(table))); 
}

// Генерация уникальной строки-ключа для блокировки конкретной таблицы в многопоточной среде
static std::string makeTableLockKey(const std::string& databaseName, const std::string& tableName)
{
    return databaseName + "." + tableName; // Формат ключа: "имя_бд.имя_таблицы"
}

// Диспетчер команд: определяет тип инструкции и перенаправляет её на выполнение
std::string DBMS::execute(const Statement& statement)
{
    if (std::holds_alternative<CreateDatabaseCommand>(statement)) return executeCreateDatabase(std::get<CreateDatabaseCommand>(statement)); // Вызов CREATE DATABASE
    if (std::holds_alternative<DropDatabaseCommand>(statement)) return executeDropDatabase(std::get<DropDatabaseCommand>(statement)); // Вызов DROP DATABASE
    if (std::holds_alternative<UseDatabaseCommand>(statement)) return executeUseDatabase(std::get<UseDatabaseCommand>(statement)); // Вызов USE
    if (std::holds_alternative<CreateTableCommand>(statement)) return executeCreateTable(std::get<CreateTableCommand>(statement)); // Вызов CREATE TABLE
    if (std::holds_alternative<DropTableCommand>(statement)) return executeDropTable(std::get<DropTableCommand>(statement)); // Вызов DROP TABLE
    if (std::holds_alternative<InsertCommand>(statement)) return executeInsert(std::get<InsertCommand>(statement)); // Вызов INSERT
    if (std::holds_alternative<UpdateCommand>(statement)) return executeUpdate(std::get<UpdateCommand>(statement)); // Вызов UPDATE
    if (std::holds_alternative<DeleteCommand>(statement)) return executeDelete(std::get<DeleteCommand>(statement)); // Вызов DELETE
    return executeSelect(std::get<SelectCommand>(statement)); // Вызов SELECT по умолчанию для оставшегося типа
}

// Выполнение команды создания новой базы данных
std::string DBMS::executeCreateDatabase(const CreateDatabaseCommand& command)
{
    std::lock_guard<std::mutex> lock(metadataMutex()); // Синхронизация потоков при изменении метаданных СУБД
    // Проверка имени базы данных на соответствие правилам идентификаторов
    if (!isValidIdentifier(command.databaseName))
    {
        throw std::runtime_error("Некорректное имя базы данных: " + command.databaseName); // Ошибка валидации имени
    }

    std::filesystem::path path = databasePath(command.databaseName); // Получение пути для новой папки БД
    // Если папка с таким именем уже физически существует
    if (std::filesystem::exists(path))
    {
        throw std::runtime_error("База данных уже существует: " + command.databaseName); // Ошибка дублирования БД
    }

    std::filesystem::create_directories(path); // Создание директории для файлов новой БД
    return "✓ База данных успешно создана: " + command.databaseName; // Возврат успешного статуса операции
}

// Выполнение команды удаления базы данных со всем содержимым
std::string DBMS::executeDropDatabase(const DropDatabaseCommand& command)
{
    std::lock_guard<std::mutex> lock(metadataMutex()); // Блокировка метаданных СУБД на время удаления
    std::filesystem::path path = databasePath(command.databaseName); // Получение пути к удаляемой папке БД
    // Если директория базы данных не найдена
    if (!std::filesystem::exists(path))
    {
        throw std::runtime_error("База данных не существует: " + command.databaseName); // Ошибка удаления отсутствующей БД
    }

    std::filesystem::remove_all(path); // Рекурсивное физическое удаление папки со всеми таблицами
    // Если удалена база данных, выбранная в текущий момент как активная
    if (currentDatabase_ == command.databaseName)
    {
        currentDatabase_.clear(); // Сброс контекста активной базы данных
    }

    return "✓ База данных успешно удалена: " + command.databaseName; // Возврат статуса об успешном удалении
}

// Выполнение команды USE для переключения контекста на указанную БД
std::string DBMS::executeUseDatabase(const UseDatabaseCommand& command)
{
    std::lock_guard<std::mutex> lock(metadataMutex()); // Потокобезопасный доступ к изменению метаданных сессии
    // Проверка физического существования папки переключаемой БД
    if (!std::filesystem::exists(databasePath(command.databaseName)))
    {
        throw std::runtime_error("База данных не существует: " + command.databaseName); 
    }

    currentDatabase_ = command.databaseName; // Фиксация нового имени активной базы данных в СУБД
    return "✓ Выбрана база данных: " + currentDatabase_; // Подтверждение успешного переключения контекста
}

// Выполнение команды создания таблицы в активной базе данных
std::string DBMS::executeCreateTable(const CreateTableCommand& command)
{
    // Проверка наличия выбранной БД перед созданием таблицы
    if (currentDatabase_.empty())
    {
        throw std::runtime_error("CREATE TABLE требует выбранную базу данных, используйте USE"); // Ошибка контекста
    }

    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(currentDatabase_, command.tableName))); // Блокировка целевой таблицы
    Database database(databasePath(currentDatabase_)); // Инициализация объекта текущей БД
    database.createTable(command.tableName, command.columns); // Создание схемы и файлов новой таблицы
    return "✓ Таблица успешно создана: " + command.tableName; // Подтверждение успешного создания таблицы
}

// Выполнение команды удаления таблицы
std::string DBMS::executeDropTable(const DropTableCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table); // Вычисление имени БД для таблицы
    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(databaseName, command.table.tableName))); // Эксклюзивная блокировка таблицы
    Database database(databasePath(databaseName)); // Инициализация объекта целевой БД
    database.dropTable(command.table); // Удаление таблицы из структуры базы данных
    return "✓ Таблица успешно удалена: " + command.table.tableName; // Подтверждение успешного удаления таблицы
}

// Выполнение команды вставки новых записей в таблицу
std::string DBMS::executeInsert(const InsertCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table); // Вычисление имени БД для вставки
    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(databaseName, command.table.tableName))); // Блокировка на запись
    Database database(databasePath(databaseName)); // Открытие целевой БД
    Table table = database.openTable(command.table); // Открытие файлов целевой таблицы
    std::size_t count = table.insertRows(command.columns, command.rows); // Выполнение операции вставки и подсчет строк
    return "✓ Вставлено строк: " + std::to_string(count); // Возврат отчета о количестве добавленных записей
}

// Выполнение команды обновления (модификации) существующих строк по условию
std::string DBMS::executeUpdate(const UpdateCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table); // Вычисление имени БД
    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(databaseName, command.table.tableName))); // Блокировка таблицы на изменение
    Database database(databasePath(databaseName)); // Открытие БД
    Table table = database.openTable(command.table); // Открытие таблицы
    std::size_t count = table.updateRows(command.assignments, command.where); // Модификация данных и возврат счетчика строк
    return "✓ Обновлено строк: " + std::to_string(count); // Возврат отчета о количестве измененных записей
}

// Выполнение команды удаления строк, соответствующих условию WHERE
std::string DBMS::executeDelete(const DeleteCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table); // Вычисление родительской БД
    std::lock_guard<std::mutex> lock(tableMutexForKey(makeTableLockKey(databaseName, command.table.tableName))); // Блокировка таблицы на удаление
    Database database(databasePath(databaseName)); // Открытие БД
    Table table = database.openTable(command.table); // Открытие таблицы
    std::size_t count = table.deleteRows(command.where); // Удаление записей с диска и возврат количества строк
    return "✓. Удалено строк: " + std::to_string(count); // Возврат отчета о количестве удаленных записей
}

// Выполнение команды выборки (поиска) данных из таблицы
std::string DBMS::executeSelect(const SelectCommand& command)
{
    std::string databaseName = resolveDatabaseName(command.table); // Определение имени целевой БД
    Database database(databasePath(databaseName)); // Открытие БД (чтение допускает параллельный доступ без блокировки)
    Table table = database.openTable(command.table); // Открытие таблицы для сканирования
    SelectResult result = table.selectRows(command.selectAll, command.items, command.where); // Извлечение данных и формирование результата
    return result.json; // Возврат итоговой выборки в формате строки JSON
}