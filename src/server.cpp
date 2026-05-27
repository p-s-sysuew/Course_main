#include "auth.h"
#include "dbms.h"
#include "heartbeat.h"
#include "logger.h"
#include "parser.h"
#include "runner.h"
#include "telemetry.h"
#include "utils.h"

#include <crow.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

// Получение порта
static int getPortFromArguments(int argc, char* argv[])
{
    int port = 8080;
    if (argc >= 2)
    {
        port = std::atoi(argv[1]);
    }
    if (port <= 0 || port > 65535)
    {
        port = 8080;
    }
    return port;
}

// JSON-ответ
static crow::response makeJsonResponse(int code, bool ok, const std::string& result, const std::string& error)
{
    crow::json::wvalue json;
    json["ok"] = ok;
    json["result"] = result;
    json["error"] = error;

    crow::response response(json);
    response.code = code;
    response.set_header("Content-Type", "application/json; charset=utf-8");
    return response;
}

// Проверка на получение ошибки из таблицы
static bool containsSqlErrorText(const std::string& text)
{
    return text.find("ERROR:") != std::string::npos;
}

// Получение токена
static std::string extractBearerToken(const crow::request& request)
{
    std::string header = request.get_header_value("Authorization");
    std::string prefix = "Bearer ";

    if (header.rfind(prefix, 0) == 0)
    {
        return header.substr(prefix.size());
    }

    return "";
}

// JSON - /login
static bool parseLoginJson(const std::string& body, std::string& user, std::string& password)
{
    crow::json::rvalue json = crow::json::load(body);
    if (!json)
    {
        return false;
    }

    if (!json.has("user") || !json.has("password"))
    {
        return false;
    }

    user = json["user"].s();
    password = json["password"].s();
    return true;
}

// JSON - /register
static bool parseRegisterJson(const std::string& body, std::string& user, std::string& password, std::string& role)
{
    crow::json::rvalue json = crow::json::load(body);
    if (!json)
    {
        return false;
    }

    if (!json.has("user") || !json.has("password"))
    {
        return false;
    }

    user = json["user"].s();
    password = json["password"].s();

    if (json.has("role"))
    {
        role = json["role"].s();
    }
    else
    {
        role.clear();
    }

    return true;
}

// Основная серверная
static void configureRoutes(crow::SimpleApp& app, DBMS& dbms, Parser& parser, Logger& logger, AuthManager& auth, HeartbeatMonitor& heartbeat)
{
    CROW_ROUTE(app, "/health").methods(crow::HTTPMethod::GET)
    ([]()
    {
        return makeJsonResponse(200, true, "ok", "");
    });

    CROW_ROUTE(app, "/register").methods(crow::HTTPMethod::POST)
    ([&auth](const crow::request& request)
    {
        std::string user;
        std::string password;
        std::string requestedRole;

        if (!parseRegisterJson(request.body, user, password, requestedRole))
        {
            return makeJsonResponse(400, false, "", "ожидался JSON {\"user\":\"...\",\"password\":\"...\",\"role\":\"reader|writer|admin\"}");
        }

        std::string creatorUser;
        std::string creatorRole;
        std::string token = extractBearerToken(request);
        if (!token.empty())
        {
            auth.verifyToken(token, creatorUser, creatorRole);
        }

        std::string error;
        if (!auth.registerUser(user, password, requestedRole, creatorRole, error))
        {
            return makeJsonResponse(409, false, "", error);
        }

        std::string newToken;
        if (!auth.login(user, password, newToken, error))
        {
            return makeJsonResponse(500, false, "", "пользователь создан, но токен не удалось выдать: " + error);
        }

        return makeJsonResponse(201, true, newToken, "");
    });

    CROW_ROUTE(app, "/login").methods(crow::HTTPMethod::POST)
    ([&auth](const crow::request& request)
    {
        std::string user;
        std::string password;
        if (!parseLoginJson(request.body, user, password))
        {
            return makeJsonResponse(400, false, "", "ожидался JSON {\"user\":\"...\",\"password\":\"...\"}");
        }

        std::string token;
        std::string error;
        if (!auth.login(user, password, token, error))
        {
            return makeJsonResponse(401, false, "", error);
        }

        return makeJsonResponse(200, true, token, "");
    });

    CROW_ROUTE(app, "/query").methods(crow::HTTPMethod::POST)
    ([&dbms, &parser, &logger, &auth](const crow::request& request)
    {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        bool errorHappened = false;

        std::string token = extractBearerToken(request);
        std::string user;
        std::string role;

        if (!auth.verifyToken(token, user, role))
        {
            globalTelemetry().record(0, true);
            return makeJsonResponse(401, false, "", "требуется корректный JWT-токен");
        }

        if (request.body.empty())
        {
            globalTelemetry().record(0, true);
            return makeJsonResponse(400, false, "", "пустое тело запроса");
        }

        std::string result;
        try
        {
            result = executeTextAuthorized(dbms, parser, logger, auth, request.body, user, "crow", role);
        }
        catch (const std::exception& ex)
        {
            errorHappened = true;
            result = ex.what();
        }

        bool sqlErrorTextFound = containsSqlErrorText(result);

        std::chrono::steady_clock::time_point finish = std::chrono::steady_clock::now();
        long long durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count();
        globalTelemetry().record(durationMs, errorHappened || sqlErrorTextFound);

        if (errorHappened || sqlErrorTextFound)
        {
            return makeJsonResponse(500, false, "", result);
        }

        return makeJsonResponse(200, true, result, "");
    });

    CROW_ROUTE(app, "/metrics").methods(crow::HTTPMethod::GET)
    ([]()
    {
        crow::response response;
        response.code = 200;
        response.set_header("Content-Type", "application/json; charset=utf-8");
        response.body = globalTelemetry().toJson();
        return response;
    });

    CROW_ROUTE(app, "/heartbeat").methods(crow::HTTPMethod::GET)
    ([&heartbeat]()
    {
        crow::response response;
        response.code = 200;
        response.set_header("Content-Type", "application/json; charset=utf-8");
        response.body = heartbeat.statusJson();
        return response;
    });
}

int main(int argc, char* argv[])
{
    int port = getPortFromArguments(argc, argv);

    DBMS dbms("data");
    Parser parser;
    Logger logger("logs");
    AuthManager auth(std::filesystem::path("data") / "_system" / "users.pb");
    HeartbeatMonitor heartbeat;
    heartbeat.loadNodes("nodes.txt");
    heartbeat.start();

    crow::SimpleApp app;
    configureRoutes(app, dbms, parser, logger, auth, heartbeat);

    std::cout << "Crow-сервер запущен на порту " << port << std::endl;
    std::cout << "POST /register, POST /login, POST /query, GET /metrics, GET /heartbeat, GET /health" << std::endl;

    app.bindaddr("0.0.0.0").port(static_cast<uint16_t>(port)).run();
    heartbeat.stop();
    return 0;
}
