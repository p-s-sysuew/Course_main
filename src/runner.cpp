#include "runner.h"
#include "utils.h"

#include <exception>
#include <sstream>
#include <vector>

// Исполнитель SQL-текста
std::string executeText(DBMS& dbms, Parser& parser, Logger& logger, const std::string& text, const std::string& clientId, const std::string& handlerId)
{
    std::ostringstream output;
    std::vector<std::string> statements = splitStatements(text);

    for (std::size_t index = 0; index < statements.size(); ++index)
    {
        std::string startTime = nowText();
        std::string status = "OK";
        std::string message;

        try
        {
            Statement statement = parser.parseStatement(statements[index]);

            if (std::holds_alternative<RegisterUserCommand>(statement))
            {
                throw std::runtime_error("Команда REGISTER требует авторизации через сервер");
            }

            message = dbms.execute(statement);
            output << message << '\n';
        }
        catch (const std::exception& error)
        {
            status = "ERROR";
            message = "✕ " + std::string(error.what());
            output << message << '\n';
        }

        std::string finishTime = nowText();
        logger.write(clientId, handlerId, statements[index], startTime, finishTime, status, message);
    }

    return output.str();
}

// Исполнитель SQL-текста + проверка роли
std::string executeTextAuthorized(DBMS& dbms, Parser& parser, Logger& logger, AuthManager& auth, const std::string& text, const std::string& clientId, const std::string& handlerId, const std::string& role)
{
    std::ostringstream output;
    std::vector<std::string> statements = splitStatements(text);

    for (std::size_t index = 0; index < statements.size(); ++index)
    {
        std::string startTime = nowText();
        std::string status = "OK";
        std::string message;

        try
        {
            Statement statement = parser.parseStatement(statements[index]);
            if (!auth.canExecute(role, statement))
            {
                throw std::runtime_error("RBAC: роль пользователя не имеет права выполнить эту команду");
            }

            if (std::holds_alternative<RegisterUserCommand>(statement))
            {
                const auto& cmd = std::get<RegisterUserCommand>(statement);
                std::string error;
                if (!auth.registerUser(cmd.username, cmd.password, cmd.role, role, error))
                {
                    throw std::runtime_error(error);
                }
                message = "✓ Пользователь успешно зарегистрирован: " + cmd.username;
            }
            else
            {
                message = dbms.execute(statement);
            }
            output << message << '\n';
        }
        catch (const std::exception& error)
        {
            status = "ERROR";
            message = "✕ " + std::string(error.what());
            output << message << '\n';
        }

        std::string finishTime = nowText();
        logger.write(clientId, handlerId, statements[index], startTime, finishTime, status, message);
    }

    return output.str();
}
