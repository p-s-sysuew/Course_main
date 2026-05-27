#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

std::string encryptBytes(const std::string& plainBytes);
std::string decryptBytes(const std::string& encryptedBytes);

void writeSecureRecords(const std::filesystem::path& path, const std::vector<std::string>& plainRecords);
void appendSecureRecord(const std::filesystem::path& path, const std::string& plainRecord, std::streamoff* writtenOffset);
std::vector<std::string> readSecureRecords(const std::filesystem::path& path);
std::vector<std::pair<std::streamoff, std::string> > readSecureRecordsWithOffsets(const std::filesystem::path& path);
std::string readSecureRecordAtOffset(const std::filesystem::path& path, std::streamoff offset);

std::streamoff secureRecordTotalSizeAtOffset(const std::filesystem::path& path, std::streamoff offset);

bool overwriteSecureRecordInSlot(
    const std::filesystem::path& path,
    const std::string& plainRecord,
    std::streamoff offset,
    std::streamoff slotTotalSize
);

bool secureFileExists(const std::filesystem::path& path);
void createEmptySecureFile(const std::filesystem::path& path);

void writeSecureLines(const std::filesystem::path& path, const std::vector<std::string>& plainLines);
void appendSecureLine(const std::filesystem::path& path, const std::string& plainLine, std::streamoff* writtenOffset);
std::vector<std::string> readSecureLines(const std::filesystem::path& path);
std::vector<std::pair<std::streamoff, std::string> > readSecureLinesWithOffsets(const std::filesystem::path& path);

