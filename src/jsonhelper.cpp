#include "jsonhelper.h" 
#include "utils.h"

#include <sstream>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define HAS_NLOHMANN 1
#else
#define HAS_NLOHMANN 0
#endif

// Перевод value в json
std::string valueToJson(const Value& value)
{
    if (value.type == ValueType::Null)
    {
        return "null";
    }

    if (value.type == ValueType::Int)
    {
        return std::to_string(value.intValue);
    }

    return "\"" + escapeJsonString(*value.stringValue) + "\"";
}

// Создание json
std::string objectsToJson(const std::vector<std::map<std::string, Value> >& objects)
{
// Если подключена библиотека
#if HAS_NLOHMANN
    nlohmann::json array = nlohmann::json::array();

    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        nlohmann::json object = nlohmann::json::object();

        std::map<std::string, Value>::const_iterator it;
        for (it = objects[objectIndex].begin(); it != objects[objectIndex].end(); ++it)
        {
            if (it->second.type == ValueType::Null) object[it->first] = nullptr;
            else if (it->second.type == ValueType::Int) object[it->first] = it->second.intValue;
            else object[it->first] = *it->second.stringValue;
        }

        array.push_back(object);
    }

    return array.dump();
#else
    std::ostringstream out;
    out << '[';

    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        if (objectIndex > 0)
        {
            out << ", ";
        }

        out << '{';
        std::size_t fieldIndex = 0;

        std::map<std::string, Value>::const_iterator it;
        for (it = objects[objectIndex].begin(); it != objects[objectIndex].end(); ++it)
        {
            if (fieldIndex > 0)
            {
                out << ", ";
            }

            out << '"' << escapeJsonString(it->first) << '"' << ": " << valueToJson(it->second);
            ++fieldIndex;
        }

        out << '}';
    }

    out << ']';
    return out.str();
#endif
}
