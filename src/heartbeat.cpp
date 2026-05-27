#include "heartbeat.h"
#include "utils.h"

#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <chrono>

// Инициализация
HeartbeatMonitor::HeartbeatMonitor()
    : running_(false)
{
}

// Остановка потока
HeartbeatMonitor::~HeartbeatMonitor()
{
    stop();
}

// Считывание nodes.txt
void HeartbeatMonitor::loadNodes(const std::string& path)
{
    nodes_.clear();
    std::ifstream input(path);
    if (!input)
    {
        return;
    }

    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        std::istringstream in(line);
        HeartbeatNode node;
        in >> node.host >> node.port;
        std::getline(in, node.restartCommand);
        node.restartCommand = trim(node.restartCommand);
        nodes_.push_back(node);
    }
}

// Запуск потока проверок
void HeartbeatMonitor::start()
{
    if (running_)
    {
        return;
    }
    running_ = true;
    worker_ = std::thread(&HeartbeatMonitor::loop, this);
}

// Остановка потока
void HeartbeatMonitor::stop()
{
    if (!running_)
    {
        return;
    }
    running_ = false;
    if (worker_.joinable())
    {
        worker_.join();
    }
}

// Непосредственно цикл проверок
void HeartbeatMonitor::loop()
{
    while (running_)
    {
        for (std::size_t index = 0; index < nodes_.size(); ++index)
        {
            bool alive = checkNode(nodes_[index]);
            if (!alive && nodes_[index].alive)
            {
                restartNode(nodes_[index]);
            }
            nodes_[index].alive = alive;
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// Обращение к /health конкретного storage-узла
bool HeartbeatMonitor::checkNode(const HeartbeatNode& node)
{
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0)
    {
        return false;
    }

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(node.port));

    if (inet_pton(AF_INET, node.host.c_str(), &address.sin_addr) <= 0)
    {
        close(clientSocket);
        return false;
    }

    if (connect(clientSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        close(clientSocket);
        return false;
    }

    std::string request = "GET /health HTTP/1.1\r\nHost: " + node.host + "\r\nConnection: close\r\n\r\n";
    send(clientSocket, request.c_str(), request.size(), 0);

    char buffer[256];
    ssize_t received = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    close(clientSocket);

    if (received <= 0)
    {
        return false;
    }

    buffer[received] = '\0';
    std::string response(buffer);
    return response.find("200") != std::string::npos;
}

// Выполнение команды перезапуска
void HeartbeatMonitor::restartNode(const HeartbeatNode& node)
{
    if (!node.restartCommand.empty())
    {
        std::system(node.restartCommand.c_str());
    }
}

// Формирование JSON
std::string HeartbeatMonitor::statusJson()
{
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < nodes_.size(); ++index)
    {
        if (index > 0)
        {
            out << ",";
        }
        out << "{\"host\":\"" << nodes_[index].host << "\",";
        out << "\"port\":" << nodes_[index].port << ",";
        out << "\"alive\":" << (nodes_[index].alive ? "true" : "false") << "}";
    }
    out << "]";
    return out.str();
}
