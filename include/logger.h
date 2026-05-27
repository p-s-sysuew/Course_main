#pragma once

#include <filesystem>
#include <string>

class Logger
{
public:
    explicit Logger(const std::filesystem::path& logDirectory);

    void write(
        const std::string& clientId,
        const std::string& handlerId,
        const std::string& queryText,
        const std::string& startTime,
        const std::string& finishTime,
        const std::string& status,
        const std::string& message
    );

private:
    std::filesystem::path logPath_;
};
