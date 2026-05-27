#pragma once

#include "types.h"

#include <map>
#include <memory>
#include <set>
#include <string>

class StringPool
{
public:
    std::shared_ptr<std::string> intern(const std::string& text);

    std::size_t uniqueCount() const;
    void clear();

    void loadFromFile();
    void saveToFile() const;
    std::string report() const;

private:
    std::map<std::string, std::weak_ptr<std::string> > pool_;
    std::set<std::string> knownStrings_;
    bool loaded_ = false;
};

StringPool& globalStringPool();

Value makeNull();
Value makeInt(int number);
Value makeString(const std::string& text);

int compareValues(const Value& left, const Value& right);

bool valueHasColumnType(const Value& value, ColumnType type);

std::string columnTypeToString(ColumnType type);
ColumnType parseColumnType(const std::string& text);
std::string valueToReadableString(const Value& value);
