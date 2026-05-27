#include "auth.h"
#include "securefile.h"
#include "storage.h"
#include "utils.h"
#include "course_storage.pb.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <vector>

static const std::string kTokenSecret = "course-work-2026-real-jwt-secret";
static const long long kTokenLifeSeconds = 24 * 60 * 60;

// Кодирование в base64Url
static std::string base64UrlEncode(const std::string& bytes)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string result;
    int value = 0;
    int bits = -6;

    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        unsigned char ch = static_cast<unsigned char>(bytes[index]);
        value = (value << 8) + ch;
        bits += 8;

        while (bits >= 0)
        {
            result.push_back(alphabet[(value >> bits) & 0x3F]);
            bits -= 6;
        }
    }

    if (bits > -6)
    {
        result.push_back(alphabet[((value << 8) >> (bits + 8)) & 0x3F]);
    }

    return result;
}

// Декодирование из base64Url
static std::string base64UrlDecode(const std::string& text)
{
    std::vector<int> table(256, -1);
    for (int index = 0; index < 26; ++index)
    {
        table[static_cast<unsigned char>('A' + index)] = index;
        table[static_cast<unsigned char>('a' + index)] = index + 26;
    }
    for (int index = 0; index < 10; ++index)
    {
        table[static_cast<unsigned char>('0' + index)] = index + 52;
    }
    table[static_cast<unsigned char>('-')] = 62;
    table[static_cast<unsigned char>('_')] = 63;

    std::string result;
    int value = 0;
    int bits = -8;

    for (std::size_t index = 0; index < text.size(); ++index)
    {
        unsigned char ch = static_cast<unsigned char>(text[index]);
        if (table[ch] == -1)
        {
            throw std::runtime_error("JWT: некорректный Base64URL-символ");
        }

        value = (value << 6) + table[ch];
        bits += 6;

        if (bits >= 0)
        {
            result.push_back(static_cast<char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }

    return result;
}

// Экранирование строк для JSON payload
static std::string jsonEscape(const std::string& text)
{
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        char ch = text[index];
        if (ch == '"') result += "\\\"";
        else if (ch == '\\') result += "\\\\";
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else if (ch == '\t') result += "\\t";
        else result.push_back(ch);
    }
    return result;
}

// JSON-парсер только для JWT payload
static bool extractJsonString(const std::string& json, const std::string& fieldName, std::string& value)
{
    std::string marker = "\"" + fieldName + "\":\"";
    std::size_t start = json.find(marker);
    if (start == std::string::npos)
    {
        return false;
    }

    start += marker.size();
    value.clear();
    bool escaped = false;

    for (std::size_t index = start; index < json.size(); ++index)
    {
        char ch = json[index];

        if (escaped)
        {
            if (ch == 'n') value.push_back('\n');
            else if (ch == 'r') value.push_back('\r');
            else if (ch == 't') value.push_back('\t');
            else value.push_back(ch);
            escaped = false;
            continue;
        }

        if (ch == '\\')
        {
            escaped = true;
            continue;
        }

        if (ch == '"')
        {
            return true;
        }

        value.push_back(ch);
    }

    return false;
}

// Получение поля exp из JWT payload.
static bool extractJsonLongLong(const std::string& json, const std::string& fieldName, long long& value)
{
    std::string marker = "\"" + fieldName + "\":";
    std::size_t start = json.find(marker);
    if (start == std::string::npos)
    {
        return false;
    }

    start += marker.size();
    std::size_t end = start;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9')
    {
        ++end;
    }

    if (end == start)
    {
        return false;
    }

    value = std::stoll(json.substr(start, end - start));
    return true;
}

// Сравнивает строки подписи
static bool constantTimeEquals(const std::string& left, const std::string& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    unsigned char diff = 0;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        diff |= static_cast<unsigned char>(left[index] ^ right[index]);
    }

    return diff == 0;
}

// Конструктор AuthManager
AuthManager::AuthManager(const std::filesystem::path& filePath)
    : filePath_(filePath)
{
    load();
    ensureDefaultUsers();
}

// Чтение users.pb
void AuthManager::load()
{
    users_.clear();

    std::vector<std::string> records = readSecureRecords(filePath_);
    if (records.empty())
    {
        return;
    }

    ProtoAuthFile authFile;
    if (!authFile.ParseFromString(records[0]))
    {
        throw std::runtime_error("auth: не удалось прочитать protobuf-файл пользователей");
    }

    for (int index = 0; index < authFile.users_size(); ++index)
    {
        const ProtoAuthUser& proto = authFile.users(index);
        AuthUser user;
        user.name = proto.name();
        user.salt = proto.salt();
        user.passwordHash = proto.password_hash();
        user.role = proto.role();
        users_[user.name] = user;
    }
}

// Сохранение пользователей
void AuthManager::save() const
{
    ProtoAuthFile authFile;
    std::map<std::string, AuthUser>::const_iterator it;
    for (it = users_.begin(); it != users_.end(); ++it)
    {
        ProtoAuthUser* proto = authFile.add_users();
        proto->set_name(it->second.name);
        proto->set_salt(it->second.salt);
        proto->set_password_hash(it->second.passwordHash);
        proto->set_role(it->second.role);
    }

    std::string bytes;
    if (!authFile.SerializeToString(&bytes))
    {
        throw std::runtime_error("auth: не удалось сериализовать пользователей в protobuf");
    }

    std::vector<std::string> records;
    records.push_back(bytes);
    writeSecureRecords(filePath_, records);
}

// Создание соли
std::string AuthManager::makeSalt(const std::string& userName) const
{
    return "salt_" + userName + "_2026";
}

// Получение хэша пароля
std::string AuthManager::hashPassword(const std::string& salt, const std::string& password) const
{
    std::hash<std::string> hasher;
    std::size_t value = hasher(salt + ":" + password);
    return std::to_string(static_cast<unsigned long long>(value));
}

// Создание стартовых пользователей, если других нет
void AuthManager::ensureDefaultUsers()
{
    if (users_.empty())
    {
        AuthUser admin;
        admin.name = "admin";
        admin.salt = makeSalt(admin.name);
        admin.passwordHash = hashPassword(admin.salt, "admin");
        admin.role = "admin";
        users_[admin.name] = admin;

        AuthUser reader;
        reader.name = "reader";
        reader.salt = makeSalt(reader.name);
        reader.passwordHash = hashPassword(reader.salt, "reader");
        reader.role = "reader";
        users_[reader.name] = reader;

        AuthUser writer;
        writer.name = "writer";
        writer.salt = makeSalt(writer.name);
        writer.passwordHash = hashPassword(writer.salt, "writer");
        writer.role = "writer";
        users_[writer.name] = writer;

        save();
    }
}

// Регистрация
bool AuthManager::registerUser(
    const std::string& userName,
    const std::string& password,
    const std::string& requestedRole,
    const std::string& creatorRole,
    std::string& errorMessage
)
{
    if (userName.empty())
    {
        errorMessage = "имя пользователя не может быть пустым";
        return false;
    }

    if (password.empty())
    {
        errorMessage = "пароль не может быть пустым";
        return false;
    }

    if (users_.find(userName) != users_.end())
    {
        errorMessage = "пользователь с таким именем уже существует";
        return false;
    }

    std::string role = requestedRole;
    if (role.empty())
    {
        role = "reader";
    }

    if (role != "reader" && role != "writer" && role != "admin")
    {
        errorMessage = "неизвестная роль: допустимы reader, writer, admin";
        return false;
    }

    if (role != "reader" && creatorRole != "admin")
    {
        errorMessage = "роль writer/admin может назначать только admin";
        return false;
    }

    AuthUser user;
    user.name = userName;
    user.salt = makeSalt(user.name);
    user.passwordHash = hashPassword(user.salt, password);
    user.role = role;

    users_[user.name] = user;
    save();
    return true;
}

// Логин
bool AuthManager::login(const std::string& userName, const std::string& password, std::string& token, std::string& errorMessage)
{
    std::map<std::string, AuthUser>::const_iterator found = users_.find(userName);
    if (found == users_.end())
    {
        errorMessage = "неизвестный пользователь";
        return false;
    }

    std::string hash = hashPassword(found->second.salt, password);
    if (hash != found->second.passwordHash)
    {
        errorMessage = "неверный пароль";
        return false;
    }

    token = makeToken(found->second.name, found->second.role);
    return true;
}

// HMAC-SHA256 от dataToSign и кодирование в Base64URL.
std::string AuthManager::signPayload(const std::string& dataToSign) const
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLength = 0;

    HMAC(
        EVP_sha256(),
        kTokenSecret.data(),
        static_cast<int>(kTokenSecret.size()),
        reinterpret_cast<const unsigned char*>(dataToSign.data()),
        dataToSign.size(),
        digest,
        &digestLength
    );

    std::string rawSignature(reinterpret_cast<char*>(digest), digestLength);
    return base64UrlEncode(rawSignature);
}

// Сбор JWT токена
std::string AuthManager::makeToken(const std::string& userName, const std::string& role) const
{
    long long expires = static_cast<long long>(std::time(nullptr)) + kTokenLifeSeconds;

    std::string header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    std::string payload = "{\"sub\":\"" + jsonEscape(userName) +
                          "\",\"role\":\"" + jsonEscape(role) +
                          "\",\"exp\":" + std::to_string(expires) + "}";

    std::string encodedHeader = base64UrlEncode(header);
    std::string encodedPayload = base64UrlEncode(payload);
    std::string dataToSign = encodedHeader + "." + encodedPayload;
    std::string signature = signPayload(dataToSign);

    return dataToSign + "." + signature;
}

// Проверка токена
bool AuthManager::verifyToken(const std::string& token, std::string& userName, std::string& role) const
{
    std::size_t firstDot = token.find('.');
    if (firstDot == std::string::npos)
    {
        return false;
    }

    std::size_t secondDot = token.find('.', firstDot + 1);
    if (secondDot == std::string::npos)
    {
        return false;
    }

    if (token.find('.', secondDot + 1) != std::string::npos)
    {
        return false;
    }

    std::string encodedHeader = token.substr(0, firstDot);
    std::string encodedPayload = token.substr(firstDot + 1, secondDot - firstDot - 1);
    std::string receivedSignature = token.substr(secondDot + 1);
    std::string dataToSign = encodedHeader + "." + encodedPayload;
    std::string expectedSignature = signPayload(dataToSign);

    if (!constantTimeEquals(receivedSignature, expectedSignature))
    {
        return false;
    }

    std::string header;
    std::string payload;
    try
    {
        header = base64UrlDecode(encodedHeader);
        payload = base64UrlDecode(encodedPayload);
    }
    catch (const std::exception&)
    {
        return false;
    }

    if (header.find("\"alg\":\"HS256\"") == std::string::npos ||
        header.find("\"typ\":\"JWT\"") == std::string::npos)
    {
        return false;
    }

    std::string subject;
    std::string extractedRole;
    long long expires = 0;

    if (!extractJsonString(payload, "sub", subject))
    {
        return false;
    }

    if (!extractJsonString(payload, "role", extractedRole))
    {
        return false;
    }

    if (!extractJsonLongLong(payload, "exp", expires))
    {
        return false;
    }

    if (expires < static_cast<long long>(std::time(nullptr)))
    {
        return false;
    }

    if (extractedRole != "reader" && extractedRole != "writer" && extractedRole != "admin")
    {
        return false;
    }

    userName = subject;
    role = extractedRole;
    return true;
}

// Проверка прав
bool AuthManager::canExecute(const std::string& role, const Statement& statement) const
{
    if (role == "admin")
    {
        return true;
    }

    if (role == "reader")
    {
        return std::holds_alternative<UseDatabaseCommand>(statement) ||
               std::holds_alternative<SelectCommand>(statement);
    }

    if (role == "writer")
    {
        return std::holds_alternative<UseDatabaseCommand>(statement) ||
               std::holds_alternative<SelectCommand>(statement) ||
               std::holds_alternative<InsertCommand>(statement) ||
               std::holds_alternative<UpdateCommand>(statement) ||
               std::holds_alternative<DeleteCommand>(statement);
    }

    return false;
}
