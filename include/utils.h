#pragma once

#include <filesystem>
#include <string>
#include <vector>

std::string trim(const std::string& text);
std::string toUpper(const std::string& text);
bool isValidIdentifier(const std::string& text);
void ensureDirectoryExists(const std::filesystem::path& path);
std::vector<std::string> splitByChar(const std::string& text, char delimiter);
std::vector<std::string> splitStatements(const std::string& text);
std::string readWholeFile(const std::filesystem::path& path);
std::string nowText();
std::string escapeJsonString(const std::string& text);

