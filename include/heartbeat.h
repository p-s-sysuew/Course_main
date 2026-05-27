#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct HeartbeatNode
{
    std::string host;
    int port = 0;
    std::string restartCommand;
    bool alive = false;
};

class HeartbeatMonitor
{
public:
    HeartbeatMonitor();
    ~HeartbeatMonitor();
    void loadNodes(const std::string& path);
    void start();
    void stop();
    std::string statusJson();

private:
    std::vector<HeartbeatNode> nodes_;
    std::thread worker_;
    std::atomic<bool> running_;

    void loop();
    bool checkNode(const HeartbeatNode& node);
    void restartNode(const HeartbeatNode& node);
};
