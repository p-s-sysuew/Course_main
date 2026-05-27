#include "telemetry.h"

#include <sstream>

// Удаление старых событий
void Telemetry::removeOldEvents(std::chrono::steady_clock::time_point now)
{
    while (!events_.empty())
    {
        long long ageSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - events_.front().time).count();
        if (ageSeconds <= 10 * 60)
        {
            break;
        }
        events_.pop_front();
    }
}

// Записать событие
void Telemetry::record(long long durationMs, bool error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TelemetryEvent event;
    event.time = std::chrono::steady_clock::now();
    event.durationMs = durationMs;
    event.error = error;
    events_.push_back(event);
    removeOldEvents(event.time);
}

// Запись метрик в JSON
std::string Telemetry::toJson()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    removeOldEvents(now);

    int requestsLastSecond = 0;
    int requestsLastMinute = 0;

    int errorsLastMinute = 0;
    int errorsLast10Minutes = 0;

    long long durationLast10Seconds = 0;
    int durationCountLast10Seconds = 0;

    int buckets[600];
    for (int index = 0; index < 600; ++index)
    {
        buckets[index] = 0;
    }

    for (std::size_t index = 0; index < events_.size(); ++index)
    {
        long long ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - events_[index].time).count();
        long long ageSec = ageMs / 1000;

        if (ageSec < 1)
        {
            ++requestsLastSecond;
        }
        if (ageSec < 60)
        {
            ++requestsLastMinute;
            if (events_[index].error)
            {
                ++errorsLastMinute;
            }
        }

        if (ageSec >= 0 && ageSec < 600 && events_[index].error)
        {
            ++errorsLast10Minutes;
        }

        if (ageSec < 10)
        {
            durationLast10Seconds += events_[index].durationMs;
            ++durationCountLast10Seconds;
        }
        if (ageSec >= 0 && ageSec < 600)
        {
            ++buckets[static_cast<int>(ageSec)];
        }
    }

    int maxRps = 0;
    int totalRequests10Minutes = 0;
    for (int index = 0; index < 600; ++index)
    {
        if (buckets[index] > maxRps)
        {
            maxRps = buckets[index];
        }
        totalRequests10Minutes += buckets[index];
    }

    double averageRps = static_cast<double>(totalRequests10Minutes) / 600.0;
    double averageDuration = 0.0;
    if (durationCountLast10Seconds > 0)
    {
        averageDuration = static_cast<double>(durationLast10Seconds) / static_cast<double>(durationCountLast10Seconds);
    }

    double errorRate = 0.0;
    if (requestsLastMinute > 0)
    {
        errorRate = static_cast<double>(errorsLastMinute) / static_cast<double>(requestsLastMinute);
    }

    std::ostringstream out;
    out << "{";
    out << "\"current_rps\":" << requestsLastSecond << ",";
    out << "\"average_rps_10_min\":" << averageRps << ",";
    out << "\"max_rps_10_min\":" << maxRps << ",";
    out << "\"average_time_ms_10_sec\":" << averageDuration << ",";
    out << "\"requests_10_min\":" << totalRequests10Minutes << ",";
    out << "\"errors_1_min\":" << errorsLastMinute << ",";
    out << "\"errors_10_min\":" << errorsLast10Minutes << ",";
    out << "\"error_rate_1_min\":" << errorRate;
    out << "}";
    return out.str();
}

// Создание объекта телеметрии
Telemetry& globalTelemetry()
{
    static Telemetry telemetry;
    return telemetry;
}
