#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

struct TelemetryEvent
{
    std::chrono::steady_clock::time_point time;
    long long durationMs = 0;
    bool error = false;
};

class Telemetry
{
public:
    void record(long long durationMs, bool error);
    std::string toJson();

private:
    std::mutex mutex_;
    std::deque<TelemetryEvent> events_;

    void removeOldEvents(std::chrono::steady_clock::time_point now);
};

Telemetry& globalTelemetry();
