#include "logger.h"
#include "securefile.h"
#include "storage.h"
#include "utils.h"
#include "course_storage.pb.h"

#include <sstream>
#include <stdexcept>

// Подготовка логов
Logger::Logger(const std::filesystem::path& logDirectory)
{
    ensureDirectoryExists(logDirectory);
    logPath_ = logDirectory / "access.pb";
    createEmptySecureFile(logPath_);
}

// Непосредственно логирование
void Logger::write(
    const std::string& clientId,
    const std::string& handlerId,
    const std::string& queryText,
    const std::string& startTime,
    const std::string& finishTime,
    const std::string& status,
    const std::string& message
)
{
    ProtoLogEntry entry;
    entry.set_client_id(clientId);
    entry.set_handler_id(handlerId);
    entry.set_start_time(startTime);
    entry.set_finish_time(finishTime);
    entry.set_status(status);
    entry.set_query_text(queryText);
    entry.set_message(message);

    std::string bytes;
    if (!entry.SerializeToString(&bytes))
    {
        throw std::runtime_error("logger: не удалось сериализовать access log в protobuf");
    }

    appendSecureRecord(logPath_, bytes, nullptr);
}
