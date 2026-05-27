#include "stringpool.h"
#include "securefile.h"
#include "storage.h"
#include "utils.h"
#include "course_storage.pb.h"

#include <filesystem>
#include <sstream>
#include <stdexcept>

// Путь.
static std::filesystem::path stringPoolFilePath()
{
    return std::filesystem::path("data") / "_system" / "string_pool.pb";
}

// Загрузка известных строк
void StringPool::loadFromFile()
{
    if (loaded_)
    {
        return;
    }

    std::vector<std::string> records = readSecureRecords(stringPoolFilePath());
    if (!records.empty())
    {
        ProtoStringPool proto;
        if (!proto.ParseFromString(records[0]))
        {
            throw std::runtime_error("stringpool: не удалось прочитать protobuf-файл пула строк");
        }

        for (int index = 0; index < proto.values_size(); ++index)
        {
            knownStrings_.insert(proto.values(index));
        }
    }

    loaded_ = true;
}

// Сохранение уникальной строки
void StringPool::saveToFile() const
{
    ProtoStringPool proto;

    std::set<std::string>::const_iterator it;
    for (it = knownStrings_.begin(); it != knownStrings_.end(); ++it)
    {
        proto.add_values(*it);
    }

    std::string bytes;
    if (!proto.SerializeToString(&bytes))
    {
        throw std::runtime_error("stringpool: не удалось сериализовать пул строк в protobuf");
    }

    std::vector<std::string> records;
    records.push_back(bytes);
    writeSecureRecords(stringPoolFilePath(), records);
}

// Получение общего объекта для строки
std::shared_ptr<std::string> StringPool::intern(const std::string& text)
{
    loadFromFile();

    std::map<std::string, std::weak_ptr<std::string> >::iterator found = pool_.find(text);
    if (found != pool_.end())
    {
        std::shared_ptr<std::string> existing = found->second.lock();
        if (existing)
        {
            return existing;
        }
    }

    std::shared_ptr<std::string> created = std::make_shared<std::string>(text);
    pool_[text] = created;

    if (knownStrings_.insert(text).second)
    {
        saveToFile();
    }

    return created;
}

// Число уникальных строк в пуле
std::size_t StringPool::uniqueCount() const
{
    return knownStrings_.size();
}

// Очистка
void StringPool::clear()
{
    pool_.clear();
    knownStrings_.clear();
    loaded_ = true;
    saveToFile();
}

// Для демонстраций возможно пригодится
std::string StringPool::report() const
{
    std::ostringstream out;
    out << "unique_strings=" << knownStrings_.size();
    return out.str();
}

// Получение общего пула
StringPool& globalStringPool()
{
    static StringPool pool;
    return pool;
}

// Создание null-значения
Value makeNull()
{
    Value value;
    value.type = ValueType::Null;
    return value;
}

// Создание int-значения
Value makeInt(int number)
{
    Value value;
    value.type = ValueType::Int;
    value.intValue = number;
    return value;
}

// Создание string-значения
Value makeString(const std::string& text)
{
    Value value;
    value.type = ValueType::String;
    value.stringValue = globalStringPool().intern(text);
    return value;
}

// Сравнение значений
int compareValues(const Value& left, const Value& right)
{
    if (left.type != right.type)
    {
        throw std::runtime_error("Ошибка compareValues: нельзя сравнить значения разных типов");
    }

    if (left.type == ValueType::Int)
    {
        if (left.intValue < right.intValue) return -1;
        if (left.intValue > right.intValue) return 1;
        return 0;
    }

    if (left.type == ValueType::String)
    {
        const std::string& a = *left.stringValue;
        const std::string& b = *right.stringValue;
        if (a < b) return -1;
        if (a > b) return 1;
        return 0;
    }

    return 0;
}

// Проверка на совместимость со столбцом
bool valueHasColumnType(const Value& value, ColumnType type)
{
    if (value.type == ValueType::Null)
    {
        return false;
    }

    if (type == ColumnType::Int)
    {
        return value.type == ValueType::Int;
    }

    return value.type == ValueType::String;
}

// Тип в строку
std::string columnTypeToString(ColumnType type)
{
    if (type == ColumnType::Int)
    {
        return "INT";
    }
    return "STRING";
}

// Строка в тип
ColumnType parseColumnType(const std::string& text)
{
    std::string upper = toUpper(text);

    if (upper == "INT")
    {
        return ColumnType::Int;
    }
    if (upper == "STRING")
    {
        return ColumnType::String;
    }

    throw std::runtime_error("Ошибка parseColumnType: неизвестный тип столбца");
}

// Int в строку
std::string valueToReadableString(const Value& value)
{
    if (value.type == ValueType::Null)
    {
        return "NULL";
    }

    if (value.type == ValueType::Int)
    {
        return std::to_string(value.intValue);
    }

    return *value.stringValue;
}
