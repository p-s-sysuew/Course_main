#pragma once

#include <mutex>
#include <string>

std::mutex& tableMutexForKey(const std::string& key);
std::mutex& metadataMutex();
