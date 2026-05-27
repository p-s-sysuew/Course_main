#include "storage.h"
#include "stringpool.h"
#include "utils.h"

#include <cstdlib>
#include <stdexcept>

// Not in use.
std::string escapeStorageString(const std::string& text)
{
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        char ch = text[index];
        if (ch == '\\') result += "\\\\";
        else if (ch == '\t') result += "\\t";
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else if (ch == '|') result += "\\p";
        else result.push_back(ch);
    }
    return result;
}

// Not in use.
std::string unescapeStorageString(const std::string& text)
{
    std::string result;
    bool escaped = false;

    for (std::size_t index = 0; index < text.size(); ++index)
    {
        char ch = text[index];
        if (!escaped)
        {
            if (ch == '\\') escaped = true;
            else result.push_back(ch);
            continue;
        }

        if (ch == 't') result.push_back('\t');
        else if (ch == 'n') result.push_back('\n');
        else if (ch == 'r') result.push_back('\r');
        else if (ch == 'p') result.push_back('|');
        else result.push_back(ch);
        escaped = false;
    }

    if (escaped)
    {
        throw std::runtime_error("некорректное экранирование строки в файле хранения");
    }

    return result;
}

// Строки в length-prefix
std::string packFields(const std::vector<std::string>& fields)
{
    std::string result;
    for (std::size_t index = 0; index < fields.size(); ++index)
    {
        result += std::to_string(fields[index].size());
        result += ':';
        result += fields[index];
    }
    return result;
}

// length-prefix в строки
std::vector<std::string> unpackFields(const std::string& text)
{
    std::vector<std::string> fields;
    std::size_t position = 0;

    while (position < text.size())
    {
        std::size_t colon = text.find(':', position);
        if (colon == std::string::npos)
        {
            throw std::runtime_error("повреждён length-prefix формат");
        }

        std::string sizeText = text.substr(position, colon - position);
        std::size_t fieldSize = static_cast<std::size_t>(std::strtoull(sizeText.c_str(), nullptr, 10));
        std::size_t fieldStart = colon + 1;

        if (fieldStart + fieldSize > text.size())
        {
            throw std::runtime_error("length-prefix поле выходит за границы строки");
        }

        fields.push_back(text.substr(fieldStart, fieldSize));
        position = fieldStart + fieldSize;
    }

    return fields;
}

// Перевод в protovalue
ProtoValue valueToProto(const Value& value)
{
    ProtoValue proto;

    if (value.type == ValueType::Null)
    {
        proto.set_kind(ProtoValue::NULL_VALUE);
    }
    else if (value.type == ValueType::Int)
    {
        proto.set_kind(ProtoValue::INT_VALUE);
        proto.set_int_value(value.intValue);
    }
    else
    {
        proto.set_kind(ProtoValue::STRING_VALUE);
        proto.set_string_value(*value.stringValue);
    }

    return proto;
}

// Перевод из protovalue
Value valueFromProto(const ProtoValue& proto, ColumnType expectedType)
{
    if (proto.kind() == ProtoValue::NULL_VALUE)
    {
        return makeNull();
    }

    if (proto.kind() == ProtoValue::INT_VALUE)
    {
        if (expectedType != ColumnType::Int)
        {
            throw std::runtime_error("protobuf value: ожидался STRING, но пришёл INT");
        }
        return makeInt(proto.int_value());
    }

    if (proto.kind() == ProtoValue::STRING_VALUE)
    {
        if (expectedType != ColumnType::String)
        {
            throw std::runtime_error("protobuf value: ожидался INT, но пришёл STRING");
        }
        return makeString(proto.string_value());
    }

    throw std::runtime_error("protobuf value: неизвестный тип значения");
}

// Перервод в protorow
std::string rowToProtoBytes(const Row& row)
{
    ProtoRow proto;
    for (std::size_t index = 0; index < row.size(); ++index)
    {
        *proto.add_values() = valueToProto(row[index]);
    }

    std::string bytes;
    if (!proto.SerializeToString(&bytes))
    {
        throw std::runtime_error("не удалось сериализовать строку таблицы в protobuf");
    }
    return bytes;
}

// Перевод из protorow
Row rowFromProtoBytes(const std::string& bytes, const std::vector<ColumnInfo>& columns)
{
    ProtoRow proto;
    if (!proto.ParseFromString(bytes))
    {
        throw std::runtime_error("не удалось прочитать protobuf-строку таблицы");
    }

    if (static_cast<std::size_t>(proto.values_size()) != columns.size())
    {
        throw std::runtime_error("количество ячеек в protobuf-строке не совпадает со схемой");
    }

    Row row;
    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        row.push_back(valueFromProto(proto.values(static_cast<int>(index)), columns[index].type));
    }
    return row;
}

// Перевод в protocolumn
static ProtoColumn::Type columnTypeToProto(ColumnType type)
{
    if (type == ColumnType::Int)
    {
        return ProtoColumn::INT;
    }
    return ProtoColumn::STRING;
}

// Перевод из protocolumn
static ColumnType columnTypeFromProto(ProtoColumn::Type type)
{
    if (type == ProtoColumn::INT)
    {
        return ColumnType::Int;
    }
    return ColumnType::String;
}

// Перевод в protoschema
std::string schemaToProtoBytes(const std::vector<ColumnInfo>& columns)
{
    ProtoSchema schema;

    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        ProtoColumn* column = schema.add_columns();
        column->set_name(columns[index].name);
        column->set_type(columnTypeToProto(columns[index].type));
        column->set_not_null(columns[index].notNull);
        column->set_indexed(columns[index].indexed);
        column->set_has_default(columns[index].hasDefault);
        *column->mutable_default_value() = valueToProto(columns[index].defaultValue);
    }

    std::string bytes;
    if (!schema.SerializeToString(&bytes))
    {
        throw std::runtime_error("не удалось сериализовать схему таблицы в protobuf");
    }
    return bytes;
}

// Перевод из protoschema
std::vector<ColumnInfo> schemaFromProtoBytes(const std::string& bytes)
{
    ProtoSchema schema;
    if (!schema.ParseFromString(bytes))
    {
        throw std::runtime_error("не удалось прочитать protobuf-схему таблицы");
    }

    std::vector<ColumnInfo> columns;
    for (int index = 0; index < schema.columns_size(); ++index)
    {
        const ProtoColumn& proto = schema.columns(index);
        ColumnInfo column;
        column.name = proto.name();
        column.type = columnTypeFromProto(proto.type());
        column.notNull = proto.not_null();
        column.indexed = proto.indexed();
        column.hasDefault = proto.has_default();
        column.defaultValue = valueFromProto(proto.default_value(), column.type);
        columns.push_back(column);
    }
    return columns;
}

// Перевод в ProtoIndexEntry
std::string indexEntryToProtoBytes(const Value& key, std::streamoff offset)
{
    ProtoIndexEntry entry;
    *entry.mutable_key() = valueToProto(key);
    entry.set_offset(static_cast<long long>(offset));

    std::string bytes;
    if (!entry.SerializeToString(&bytes))
    {
        throw std::runtime_error("не удалось сериализовать запись индекса в protobuf");
    }
    return bytes;
}

// Перевод из ProtoIndexEntry
void indexEntryFromProtoBytes(const std::string& bytes, ColumnType type, Value& key, std::streamoff& offset)
{
    ProtoIndexEntry entry;
    if (!entry.ParseFromString(bytes))
    {
        throw std::runtime_error("не удалось прочитать protobuf-запись индекса");
    }

    key = valueFromProto(entry.key(), type);
    offset = static_cast<std::streamoff>(entry.offset());
}

// - - - Старые функции - - -

std::string serializeValue(const Value& value)
{
    if (value.type == ValueType::Null) return "N";
    if (value.type == ValueType::Int) return "I:" + std::to_string(value.intValue);
    std::vector<std::string> fields;
    fields.push_back(*value.stringValue);
    return "S:" + packFields(fields);
}

Value deserializeValue(const std::string& text, ColumnType expectedType)
{
    if (text == "N") return makeNull();
    if (text.size() < 2 || text[1] != ':') throw std::runtime_error("повреждённая текстовая ячейка");

    if (text[0] == 'I')
    {
        if (expectedType != ColumnType::Int) throw std::runtime_error("тип текстовой ячейки не совпадает со схемой");
        return makeInt(std::stoi(text.substr(2)));
    }

    if (text[0] == 'S')
    {
        if (expectedType != ColumnType::String) throw std::runtime_error("тип текстовой ячейки не совпадает со схемой");
        std::string body = text.substr(2);
        std::vector<std::string> fields = unpackFields(body);
        if (fields.size() == 1) return makeString(fields[0]);
        return makeString(unescapeStorageString(body));
    }

    throw std::runtime_error("неизвестный префикс текстовой ячейки");
}

std::string serializeRow(const Row& row)
{
    std::vector<std::string> fields;
    for (std::size_t index = 0; index < row.size(); ++index)
    {
        fields.push_back(serializeValue(row[index]));
    }
    return "ROW:" + packFields(fields);
}

Row deserializeRow(const std::string& line, const std::vector<ColumnInfo>& columns)
{
    std::vector<std::string> parts;
    if (line.rfind("ROW:", 0) == 0) parts = unpackFields(line.substr(4));
    else parts = splitByChar(line, '\t');

    if (parts.size() != columns.size()) throw std::runtime_error("количество текстовых ячеек не совпадает со схемой");

    Row row;
    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        row.push_back(deserializeValue(parts[index], columns[index].type));
    }
    return row;
}

std::string serializeDefault(const ColumnInfo& column)
{
    if (!column.hasDefault) return "";
    return serializeValue(column.defaultValue);
}

ColumnInfo parseSchemaLine(const std::string& line)
{
    std::vector<std::string> parts;
    if (line.rfind("SCH:", 0) == 0) parts = unpackFields(line.substr(4));
    else parts = splitByChar(line, '|');

    if (parts.size() != 4 && parts.size() != 6) throw std::runtime_error("повреждённая текстовая схема");

    ColumnInfo column;
    column.name = parts[0];
    column.type = parseColumnType(parts[1]);
    column.notNull = parts[2] == "1";
    column.indexed = parts[3] == "1";
    if (parts.size() == 6)
    {
        column.hasDefault = parts[4] == "1";
        if (column.hasDefault) column.defaultValue = deserializeValue(parts[5], column.type);
    }
    return column;
}

std::string makeSchemaLine(const ColumnInfo& column)
{
    std::vector<std::string> fields;
    fields.push_back(column.name);
    fields.push_back(columnTypeToString(column.type));
    fields.push_back(column.notNull ? "1" : "0");
    fields.push_back(column.indexed ? "1" : "0");
    fields.push_back(column.hasDefault ? "1" : "0");
    fields.push_back(serializeDefault(column));
    return "SCH:" + packFields(fields);
}

