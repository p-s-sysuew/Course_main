#pragma once

#include "types.h"

#include <filesystem>
#include <map>
#include <string>

struct AuthUser
{
    std::string name;
    std::string salt;
    std::string passwordHash;
    std::string role;
};

class AuthManager
{
public:
    explicit AuthManager(const std::filesystem::path& filePath);

    bool login(const std::string& userName, const std::string& password, std::string& token, std::string& errorMessage);

    bool registerUser(
        const std::string& userName,
        const std::string& password,
        const std::string& requestedRole,
        const std::string& creatorRole,
        std::string& errorMessage
    );

    bool verifyToken(const std::string& token, std::string& userName, std::string& role) const;

    bool canExecute(const std::string& role, const Statement& statement) const;

    void ensureDefaultUsers();

private:
    std::filesystem::path filePath_;
    std::map<std::string, AuthUser> users_;

    void load();
    void save() const;
    std::string makeSalt(const std::string& userName) const;
    std::string hashPassword(const std::string& salt, const std::string& password) const;

    std::string makeToken(const std::string& userName, const std::string& role) const;

    std::string signPayload(const std::string& dataToSign) const;
};
