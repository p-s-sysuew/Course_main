#include "dbms.h"
#include "logger.h"
#include "parser.h"
#include "runner.h"
#include "utils.h"

#include <iostream>
#include <string>

// Интерактивный режим

static void interactiveMode(DBMS& dbms, Parser& parser, Logger& logger)
{
    std::cout << "Учебная СУБД запущена" << std::endl;
    std::cout << "Введите EXIT; или QUIT; для выхода" << std::endl;

    std::string buffer;
    std::string line;

    while (true)
    {
        if (buffer.empty()) std::cout << "> ";
        else std::cout << ". ";

        if (!std::getline(std::cin, line))
        {
            break;
        }

        buffer += line;
        buffer += '\n';

        std::string upper = toUpper(trim(buffer));
        if (upper == "EXIT;" || upper == "QUIT;")
        {
            break;
        }

        try
        {
            std::vector<std::string> statements = splitStatements(buffer);
            if (!statements.empty())
            {
                std::cout << executeText(dbms, parser, logger, buffer, "terminal", "main") << std::flush;
                buffer.clear();
            }
        }
        catch (const std::exception&)
        {
            // ! команда ещё может быть не завершена символом ";", ждём следующую строку
        }
    }
}

int main(int argc, char* argv[])
{
    try
    {
        DBMS dbms("data");
        dbms.setLogPath("logs/access.pb");
        Parser parser;
        Logger logger("logs");

        if (argc == 1)
        {
            interactiveMode(dbms, parser, logger);
            return 0;
        }

        // Пакетный режим, чтение файла
        if (argc == 2)
        {
            std::string text = readWholeFile(argv[1]);
            std::cout << executeText(dbms, parser, logger, text, "script", "main") << std::flush;
            return 0;
        }

        std::cerr << "Использование:" << std::endl;
        std::cerr << "./course_work" << std::endl;
        std::cerr << "./course_work script.sql" << std::endl;
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Критическая ошибка: " << error.what() << std::endl;
        return 1;
    }
}
