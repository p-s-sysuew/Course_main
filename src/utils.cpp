#include "utils.h"

#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

// Удаление лишних пробелов 
std::string trim(const std::string& text)
{
    std::size_t left = 0;
    while (left < text.size() && std::isspace(static_cast<unsigned char>(text[left])) != 0)
    {
        ++left;
    }

    std::size_t right = text.size();
    while (right > left && std::isspace(static_cast<unsigned char>(text[right - 1])) != 0)
    {
        --right;
    }

    return text.substr(left, right - left);
}

// Перевод строки в верхний регистр
std::string toUpper(const std::string& text)
{
    std::string result = text;
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        result[index] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[index])));
    }
    return result;
}

// Валидация имён
bool isValidIdentifier(const std::string& text)
{
    if (text.empty())
    {
        return false;
    }

    if (std::isalpha(static_cast<unsigned char>(text[0])) == 0 && text[0] != '_')
    {
        return false;
    }

    for (std::size_t index = 0; index < text.size(); ++index)
    {
        char ch = text[index];
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_')
        {
            return false;
        }
    }

    return true;
}

// Создание отсутствующей папки
void ensureDirectoryExists(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
    {
        std::filesystem::create_directories(path);
    }
}

// Разделение строки по символу-разделителю
std::vector<std::string> splitByChar(const std::string& text, char delimiter)
{
    std::vector<std::string> parts;
    std::string current;

    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] == delimiter)
        {
            parts.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(text[index]);
        }
    }

    parts.push_back(current);
    return parts;
}

// Разбиение текста на команды
std::vector<std::string> splitStatements(const std::string& text)
{
    std::vector<std::string> result;
    std::string current;
    bool inString = false;
    bool escaped = false;

    for (std::size_t index = 0; index < text.size(); ++index)
    {
        char ch = text[index];
        current.push_back(ch);

        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
            continue;
        }

        if (ch == ';')
        {
            std::string statement = trim(current);
            if (!statement.empty())
            {
                result.push_back(statement);
            }
            current.clear();
        }
    }

    if (!trim(current).empty())
    {
        throw std::runtime_error("Ошибка splitStatements: последняя команда не завершена символом ;");
    }

    return result;
}

// Считывание всего файла в строку
std::string readWholeFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input)
    {
        throw std::runtime_error("Ошибка readWholeFile: не удалось открыть файл: " + path.string());
    }

    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

// Получение текущего времени строкой
std::string nowText()
{
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    std::time_t rawTime = std::chrono::system_clock::to_time_t(now);
    std::tm tmValue;
    localtime_r(&rawTime, &tmValue);

    std::ostringstream out;
    out << std::put_time(&tmValue, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

// Обезопашивание строки для json
std::string escapeJsonString(const std::string& text)
{
    std::string result;

    for (std::size_t index = 0; index < text.size(); ++index)
    {
        char ch = text[index];
        if (ch == '\\') result += "\\\\";
        else if (ch == '"') result += "\\\"";
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else if (ch == '\t') result += "\\t";
        else result.push_back(ch);
    }

    return result;
}
