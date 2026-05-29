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

// Извлечение тела ответа путем удаления HTTP-заголовков
static std::string stripHeaders(const std::string& response)
{
    // Поиск границы между заголовками и телом (двойной перевод строки)
    std::size_t position = response.find("\r\n\r\n");
    
    // Если граница не найдена, возвращаем ответ целиком
    if (position == std::string::npos)
    {
        return response;
    }
    
    // Вырезаем и возвращаем всё, что идет после "\r\n\r\n" (длина маркера — 4 байта)
    return response.substr(position + 4);
}

// Низкоуровневая отправка HTTP-запроса и чтение ответа через TCP-сокет
static std::string sendRawHttp(const std::string& host, int port, const std::string& request)
{
    // Создаем сетевой сокет (IPv4, потоковый TCP-протокол)
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0)
    {
        throw std::runtime_error("не удалось создать сокет");
    }

    // Инициализируем структуру с адресом сервера
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address)); // Обнуляем память структуры
    address.sin_family = AF_INET;               // Устанавливаем семейство адресов IPv4
    address.sin_port = htons(static_cast<uint16_t>(port)); // Конвертируем порт в сетевой порядок байт

    // Преобразуем IP-адрес из текстового формата в бинарный
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) <= 0)
    {
        close(clientSocket); // Закрываем сокет во избежание утечки дескрипторов
        throw std::runtime_error("некорректный адрес сервера");
    }

    // Устанавливаем TCP-соединение с удаленным сервером
    if (connect(clientSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        close(clientSocket);
        throw std::runtime_error("не удалось подключиться к серверу");
    }

    // Отправляем сформированный HTTP-запрос в сокет
    if (send(clientSocket, request.c_str(), request.size(), 0) < 0)
    {
        close(clientSocket);
        throw std::runtime_error("не удалось отправить данные на сервер");
    }

    // Буфер и строка для накопления входящих данных от сервера
    std::string response;
    char buffer[4096];
    
    // Цикл последовательного чтения данных из сокета
    while (true)
    {
        // Читаем порцию данных в буфер
        ssize_t received = recv(clientSocket, buffer, sizeof(buffer), 0);
        
        // Если пришла ошибка или сервер закрыл соединение (0), выходим из цикла
        if (received <= 0)
        {
            break;
        }
        
        // Дописываем прочитанные байты в результирующую строку
        response.append(buffer, static_cast<std::size_t>(received));
    }

    // Закрываем сокет после завершения обмена данными
    close(clientSocket);
    
    // Возвращаем очищенное от HTTP-заголовков тело ответа
    return stripHeaders(response);
}

// Формирование сырого текста HTTP POST запроса
static std::string makePostRequest(const std::string& host, const std::string& path, const std::string& body, const std::string& token)
{
    std::ostringstream request;
    
    // Формируем стартовую строку HTTP и обязательные заголовки
    request << "POST " << path << " HTTP/1.1\r\n";
    request << "Host: " << host << "\r\n";
    request << "Content-Type: application/json; charset=utf-8\r\n";
    
    // Если передан токен авторизации, добавляем заголовок Bearer-микросхемы
    if (!token.empty())
    {
        request << "Authorization: Bearer " << token << "\r\n";
    }
    
    // Указываем размер тела запроса в байтах и закрываем соединение после ответа
    request << "Content-Length: " << body.size() << "\r\n";
    request << "Connection: close\r\n";
    request << "\r\n"; // Пустая строка — индикатор окончания заголовков
    request << body;   // Добавляем само JSON-тело
    
    return request.str();
}

// Быстрый ручной парсинг значения текстового поля "result" из JSON-строки
static std::string extractResultField(const std::string& json)
{
    // Маркер, с которого начинается поиск нужного поля
    std::string marker = "\"result\":\"";
    std::size_t start = json.find(marker);
    if (start == std::string::npos)
    {
        return ""; // Маркер не найден
    }
    
    // Сдвигаем указатель на начало самого значения (после кавычки маркера)
    start += marker.size();
    
    // Ищем закрывающую кавычку значения поля
    std::size_t end = json.find('"', start);
    if (end == std::string::npos)
    {
        return ""; // Закрывающая кавычка отсутствует
    }
    
    // Вырезаем подстроку между кавычками
    return json.substr(start, end - start);
}

// Авторизация на сервере по логину/паролю для получения JWT-токена
static std::string login(const std::string& host, int port, const std::string& user, const std::string& password)
{
    // Вручную склеиваем JSON с учетными данными
    std::string body = "{\"user\":\"" + user + "\",\"password\":\"" + password + "\"}";
    
    // Отправляем запрос на эндпоинт /login
    std::string response = sendRawHttp(host, port, makePostRequest(host, "/login", body, ""));
    
    // Вытаскиваем токен из ответа сервера и возвращаем его
    return extractResultField(response);
}

// Главная функция управления логикой утилиты
int main(int argc, char* argv[])
{
    // Валидация количества переданных аргументов командной строки
    if (argc < 4)
    {
        std::cerr << "Неверное использование клиентской части, правильный синтаксис:" << std::endl;
        std::cerr << "  ./course_client 127.0.0.1 8080 script.sql" << std::endl;
        std::cerr << "  ./course_client 127.0.0.1 8080 script.sql admin admin" << std::endl;
        return 1; // Завершение программы с кодом ошибки
    }

    try
    {
        // Инициализация параметров из аргументов CLI
        std::string host = argv[1];             // IP-адрес сервера
        int port = std::atoi(argv[2]);          // Порт сервера (конвертируем из текста в int)
        std::string third = argv[3];            // Путь к файлу скрипта или сам SQL-текст
        std::string user = argc >= 5 ? argv[4] : "admin";       // Логин (по дефолту admin)
        std::string password = argc >= 6 ? argv[5] : "admin";   // Пароль (по дефолту admin)
        std::string body;                       // Переменная для хранения текста запроса

        // Проверяем, является ли третий аргумент существующим файлом на диске
        if (std::filesystem::exists(third))
        {
            body = readWholeFile(third); // Читаем весь SQL-скрипт из файла
        }
        else
        {
            body = third; // Если файла нет, трактуем аргумент как чистый SQL-запрос
        }

        // Выполняем аутентификацию на сервере
        std::string token = login(host, port, user, password);
        if (token.empty())
        {
            throw std::runtime_error("сервер не выдал токен");
        }

        // Отправляем основной SQL-запрос на выполнение, передавая полученный токен
        std::string response = sendRawHttp(host, port, makePostRequest(host, "/query", body, token));
        
        // Печатаем ответ сервера (результат запроса) в стандартный вывод
        std::cout << response << std::flush << std::endl;
        return 0; // Успешное завершение программы
    }
    catch (const std::exception& error)
    {
        // Перехват и логирование любых исключений, возникших в процессе работы
        std::cerr << "Ошибка клиента: " << error.what() << std::endl;
        return 1; // Завершение с кодом ошибки
    }
}