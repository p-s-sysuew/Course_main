#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Удаляет пробелы в начале и конце строки
std::string trim(const std::string& text);

// Переводит строку в верхний регистр (для регистронезависимых ключевых слов)
std::string toUpper(const std::string& text);

// Проверяет корректность имени базы, таблицы или столбца
bool isValidIdentifier(const std::string& text);

// Создаёт директорию (включая родительские), если она не существует
void ensureDirectoryExists(const std::filesystem::path& path);

// Разбивает строку по заданному разделителю
std::vector<std::string> splitByChar(const std::string& text, char delimiter);

// Разбивает SQL-текст на отдельные команды по символу ';'
std::vector<std::string> splitStatements(const std::string& text);

// Читает весь файл в строку
std::string readWholeFile(const std::filesystem::path& path);

// Возвращает текущее время в формате "YYYY-MM-DD HH:MM:SS"
std::string nowText();

// Экранирует специальные символы для JSON
std::string escapeJsonString(const std::string& text);