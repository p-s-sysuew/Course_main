#include "utils.h"

#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

// Удаляет пробельные символы в начале и конце строки.
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

// Переводит всю строку в верхний регистр
std::string toUpper(const std::string& text)
{
    std::string result = text;
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        result[index] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[index])));
    }
    return result;
}

// Проверяет, является ли строка корректным идентификатором.
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

//Создаёт директорию (и все родительские), если она не существует
void ensureDirectoryExists(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
    {
        std::filesystem::create_directories(path);
    }
}

// Разбивает строку по указанному символу-разделителю.
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

/**
 * Разбивает SQL-текст на отдельные команды.
 * 
 * Основные возможности:
 * - Поддержка многострочных команд
 * - Команды завершаются символом ';'
 * - Корректная обработка строковых литералов в двойных кавычках ("...")
 * - Учёт экранирования внутри строк (\", \n, \t и т.д.)
 * - trim() каждой команды
 */
std::vector<std::string> splitStatements(const std::string& text)
{
    std::vector<std::string> result;
    std::string current;           // Буфер для accumulating текущей команды
    bool inString = false;         // Находимся ли мы внутри строкового литерала "..."
    bool escaped = false;          // Был ли предыдущий символ '\'

    for (std::size_t index = 0; index < text.size(); ++index)
    {
        char ch = text[index];
        current.push_back(ch);

        // Обработка символов внутри строки
        if (inString)
        {
            if (escaped)
            {
                // Предыдущий символ был '\', текущий — экранированный
                escaped = false;
            }
            else if (ch == '\\')
            {
                // Начало экранированной последовательности
                escaped = true;
            }
            else if (ch == '"')
            {
                // Закрытие строки
                inString = false;
            }
            continue;
        }

        // Начало новой строки
        if (ch == '"')
        {
            inString = true;
            continue;
        }

        // Завершение команды (только вне строкового литерала)
        if (ch == ';')
        {
            std::string statement = trim(current);
            if (!statement.empty())
            {
                result.push_back(statement);
            }
            current.clear();  // Начинаем новую команду
        }
    }

    // Проверка: если после обработки остался незавершённый текст — ошибка
    if (!trim(current).empty())
    {
        throw std::runtime_error("Ошибка splitStatements: последняя команда не завершена символом ;");
    }

    return result;
}


// Читает содержимое всего файла в строку
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

// Возвращает текущее локальное время в формате "YYYY-MM-DD HH:MM:SS"
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

// Экранирует специальные символы для безопасной вставки в JSON
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