#pragma once 

#include "types.h"

#include <map>
#include <string>
#include <vector>

std::string valueToJson(const Value& value);

std::string objectsToJson(const std::vector<std::map<std::string, Value> >& objects);
