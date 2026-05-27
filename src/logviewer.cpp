#include "course_storage.pb.h"
#include "securefile.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Файл мы зашифровали >_<

static void printSeparator()
{
    std::cout << "------------------------------------------------------------" << std::endl;
}

static void printLogEntry(const ProtoLogEntry& entry, int number)
{
    printSeparator();
    std::cout << "Запись #" << number << std::endl;
    std::cout << "Статус: " << entry.status() << std::endl;
    std::cout << "Клиент: " << entry.client_id() << std::endl;
    std::cout << "Обработчик: " << entry.handler_id() << std::endl;
    std::cout << "Начало: " << entry.start_time() << std::endl;
    std::cout << "Конец: " << entry.finish_time() << std::endl;
    std::cout << "SQL:" << std::endl;
    std::cout << entry.query_text() << std::endl;
    std::cout << "Сообщение:" << std::endl;
    std::cout << entry.message() << std::endl;
}

static void printUsage(const char* programName)
{
    std::cout << "Использование:" << std::endl;
    std::cout << "  " << programName << std::endl;
    std::cout << "  " << programName << " logs/access.pb" << std::endl;
}

int main(int argc, char* argv[])
{
    try
    {
        if (argc > 2)
        {
            printUsage(argv[0]);
            return 1;
        }

        std::filesystem::path logPath = std::filesystem::path("logs") / "access.pb";
        if (argc == 2)
        {
            logPath = argv[1];
        }

        if (!std::filesystem::exists(logPath))
        {
            std::cerr << "Файл логов не найден: " << logPath.string() << std::endl;
            std::cerr << "Сначала выполните несколько запросов через course_work или course_server" << std::endl;
            return 1;
        }

        std::vector<std::string> records = readSecureRecords(logPath);
        if (records.empty())
        {
            std::cout << "Лог пуст: " << logPath.string() << std::endl;
            return 0;
        }

        for (std::size_t index = 0; index < records.size(); ++index)
        {
            ProtoLogEntry entry;
            if (!entry.ParseFromString(records[index]))
            {
                std::cerr << "Не удалось разобрать protobuf-запись лога #" << (index + 1) << std::endl;
                continue;
            }

            printLogEntry(entry, static_cast<int>(index + 1));
        }

        printSeparator();
        std::cout << "Всего записей: " << records.size() << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Ошибка просмотра логов: " << error.what() << std::endl;
        return 1;
    }
}
