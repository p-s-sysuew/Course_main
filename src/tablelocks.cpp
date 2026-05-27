#include "tablelocks.h"

#include <map>
#include <memory>

// Мьютекс для защиты мьютексов
static std::mutex& registryMutex()
{
    static std::mutex mutex;
    return mutex;
}

// Хранение мьютекса для каждой таблицы
static std::map<std::string, std::shared_ptr<std::mutex> >& registry()
{
    static std::map<std::string, std::shared_ptr<std::mutex> > data;
    return data;
}

// Возврат мьютекса под конкретную таблицу (используя key)
std::mutex& tableMutexForKey(const std::string& key)
{
    std::lock_guard<std::mutex> lock(registryMutex());
    std::map<std::string, std::shared_ptr<std::mutex> >& data = registry();

    if (data.find(key) == data.end())
    {
        data[key] = std::shared_ptr<std::mutex>(new std::mutex());
    }

    return *data[key];
}

// Большой мьютекс
std::mutex& metadataMutex()
{
    static std::mutex mutex;
    return mutex;
}
