#pragma once

#include "auth.h"
#include "dbms.h"
#include "logger.h"
#include "parser.h"

#include <string>

std::string executeText(DBMS& dbms, Parser& parser, Logger& logger, const std::string& text, const std::string& clientId, const std::string& handlerId);
std::string executeTextAuthorized(DBMS& dbms, Parser& parser, Logger& logger, AuthManager& auth, const std::string& text, const std::string& clientId, const std::string& handlerId, const std::string& role);
