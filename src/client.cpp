#include "utils.h"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <cstdint>

// Вычет http заголовков
static std::string stripHeaders(const std::string& response)
{
    std::size_t position = response.find("\r\n\r\n");
    if (position == std::string::npos)
    {
        return response;
    }
    return response.substr(position + 4);
}

// http отправка-получение
static std::string sendRawHttp(const std::string& host, int port, const std::string& request)
{
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0)
    {
        throw std::runtime_error("не удалось создать сокет");
    }

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) <= 0)
    {
        close(clientSocket);
        throw std::runtime_error("некорректный адрес сервера");
    }

    if (connect(clientSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        close(clientSocket);
        throw std::runtime_error("не удалось подключиться к серверу");
    }

    if (send(clientSocket, request.c_str(), request.size(), 0) < 0)
    {
        close(clientSocket);
        throw std::runtime_error("не удалось отправить данные на сервер");
    }

    std::string response;
    char buffer[4096];
    while (true)
    {
        ssize_t received = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (received <= 0)
        {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }

    close(clientSocket);
    return stripHeaders(response);
}

// Сбор POSTа
static std::string makePostRequest(const std::string& host, const std::string& path, const std::string& body, const std::string& token)
{
    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n";
    request << "Host: " << host << "\r\n";
    request << "Content-Type: application/json; charset=utf-8\r\n";
    if (!token.empty())
    {
        request << "Authorization: Bearer " << token << "\r\n";
    }
    request << "Content-Length: " << body.size() << "\r\n";
    request << "Connection: close\r\n";
    request << "\r\n";
    request << body;
    return request.str();
}

// Получение поля результата из JSON
static std::string extractResultField(const std::string& json)
{
    std::string marker = "\"result\":\"";
    std::size_t start = json.find(marker);
    if (start == std::string::npos)
    {
        return "";
    }
    start += marker.size();
    std::size_t end = json.find('"', start);
    if (end == std::string::npos)
    {
        return "";
    }
    return json.substr(start, end - start);
}

// Получение токена
static std::string login(const std::string& host, int port, const std::string& user, const std::string& password)
{
    std::string body = "{\"user\":\"" + user + "\",\"password\":\"" + password + "\"}";
    std::string response = sendRawHttp(host, port, makePostRequest(host, "/login", body, ""));
    return extractResultField(response);
}

int main(int argc, char* argv[])
{
    if (argc < 4)
    {
        std::cerr << "Неверное использование клиентской части, правильный синтаксис:" << std::endl;
        std::cerr << "  ./course_client 127.0.0.1 8080 script.sql" << std::endl;
        std::cerr << "  ./course_client 127.0.0.1 8080 script.sql admin admin" << std::endl;
        return 1;
    }

    try
    {
        std::string host = argv[1];
        int port = std::atoi(argv[2]);
        std::string third = argv[3];
        std::string user = argc >= 5 ? argv[4] : "admin";
        std::string password = argc >= 6 ? argv[5] : "admin";
        std::string body;

        if (std::filesystem::exists(third))
        {
            body = readWholeFile(third);
        }
        else
        {
            body = third;
        }

        std::string token = login(host, port, user, password);
        if (token.empty())
        {
            throw std::runtime_error("сервер не выдал токен");
        }

        std::string response = sendRawHttp(host, port, makePostRequest(host, "/query", body, token));
        std::cout << response << std::flush << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Ошибка клиента: " << error.what() << std::endl;
        return 1;
    }
}
