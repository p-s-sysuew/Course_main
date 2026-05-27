#pragma once

#include "types.h"
#include "course_storage.pb.h"

#include <string>
#include <vector>

std::string escapeStorageString(const std::string& text);
std::string unescapeStorageString(const std::string& text);

std::string packFields(const std::vector<std::string>& fields);
std::vector<std::string> unpackFields(const std::string& text);

ProtoValue valueToProto(const Value& value);
Value valueFromProto(const ProtoValue& proto, ColumnType expectedType);

std::string rowToProtoBytes(const Row& row);
Row rowFromProtoBytes(const std::string& bytes, const std::vector<ColumnInfo>& columns);

std::string schemaToProtoBytes(const std::vector<ColumnInfo>& columns);
std::vector<ColumnInfo> schemaFromProtoBytes(const std::string& bytes);

std::string indexEntryToProtoBytes(const Value& key, std::streamoff offset);
void indexEntryFromProtoBytes(const std::string& bytes, ColumnType type, Value& key, std::streamoff& offset);

std::string serializeValue(const Value& value);
Value deserializeValue(const std::string& text, ColumnType expectedType);
std::string serializeRow(const Row& row);
Row deserializeRow(const std::string& line, const std::vector<ColumnInfo>& columns);
std::string serializeDefault(const ColumnInfo& column);
ColumnInfo parseSchemaLine(const std::string& line);
std::string makeSchemaLine(const ColumnInfo& column);

