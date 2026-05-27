#include "securefile.h"
#include "utils.h"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <ostream>

// To-Do: Придумать какое-нибудь хорошее название для ключа
static const std::string kStorageKey = "course-work-2026-protobuf-key";

// XOR (де)шифрования
static std::string xorWithKey(const std::string& bytes)
{
    std::string result = bytes;
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        result[index] = static_cast<char>(result[index] ^ kStorageKey[index % kStorageKey.size()]);
    }
    return result;
}

std::string encryptBytes(const std::string& plainBytes)
{
    return xorWithKey(plainBytes);
}
std::string decryptBytes(const std::string& encryptedBytes)
{
    return xorWithKey(encryptedBytes);
}

// Пометка для нового формата - Course Work ReCord
// Old: block=protobuf
// New: block="CWRC"+length+protobuf+padding
static const std::string kRecordMagic = "CWRC";

// Запись uint32_t в строку (little-endian)
static void appendUint32(std::string& output, std::uint32_t value)
{
    output.push_back(static_cast<char>(value & 0xFF));
    output.push_back(static_cast<char>((value >> 8) & 0xFF));
    output.push_back(static_cast<char>((value >> 16) & 0xFF));
    output.push_back(static_cast<char>((value >> 24) & 0xFF));
}

// Чтение uint32_t из строки
static std::uint32_t readUint32FromString(const std::string& text, std::size_t position)
{
    if (position + 4 > text.size())
    {
        throw std::runtime_error("securefile: повреждена внутренняя длина protobuf-записи");
    }

    std::uint32_t value = 0;
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[position]));
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[position + 1])) << 8;
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[position + 2])) << 16;
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[position + 3])) << 24;
    return value;
}

// Создание обёртки в стиле securefile
static std::string wrapPlainRecord(const std::string& plainRecord, std::size_t requiredPayloadSize)
{
    if (plainRecord.size() > 0xFFFFFFFFu)
    {
        throw std::runtime_error("securefile: protobuf-запись слишком большая для внутренней обёртки");
    }

    std::string wrapped;
    wrapped.reserve(kRecordMagic.size() + 4 + plainRecord.size());
    wrapped += kRecordMagic;
    appendUint32(wrapped, static_cast<std::uint32_t>(plainRecord.size()));
    wrapped += plainRecord;

    if (requiredPayloadSize == 0)
    {
        return wrapped;
    }

    if (wrapped.size() > requiredPayloadSize)
    {
        throw std::runtime_error("securefile: protobuf-запись не помещается в tombstone-слот");
    }

    wrapped.resize(requiredPayloadSize, '\0');
    return wrapped;
}

// Убрать обёртку (если в новом формате)
static std::string unwrapPlainRecord(const std::string& decryptedRecord)
{
    if (decryptedRecord.rfind(kRecordMagic, 0) != 0)
    {
        return decryptedRecord;
    }

    const std::size_t payloadSizePosition = kRecordMagic.size();
    const std::size_t payloadPosition = payloadSizePosition + 4;
    const std::uint32_t payloadSize = readUint32FromString(decryptedRecord, payloadSizePosition);

    if (payloadPosition + payloadSize > decryptedRecord.size())
    {
        throw std::runtime_error("securefile: protobuf payload выходит за границы tombstone-слота");
    }

    return decryptedRecord.substr(payloadPosition, payloadSize);
}

// Ручная запись uint32_t в поток (little-endian)
static void writeUint32(std::ostream& output, std::uint32_t value)
{
    char bytes[4];
    bytes[0] = static_cast<char>(value & 0xFF);
    bytes[1] = static_cast<char>((value >> 8) & 0xFF);
    bytes[2] = static_cast<char>((value >> 16) & 0xFF);
    bytes[3] = static_cast<char>((value >> 24) & 0xFF);
    output.write(bytes, 4);
}

// Чтение uint32_t из потока
static bool readUint32(std::ifstream& input, std::uint32_t& value)
{
    char bytes[4];
    input.read(bytes, 4);

    if (input.gcount() == 0)
    {
        return false;
    }

    if (input.gcount() != 4)
    {
        throw std::runtime_error("securefile: повреждённый бинарный файл, неполная длина записи");
    }

    value = 0;
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0]));
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 8;
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 16;
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3])) << 24;
    return true;
}

// Шифрование записей
void writeSecureRecords(const std::filesystem::path& path, const std::vector<std::string>& plainRecords)
{
    ensureDirectoryExists(path.parent_path());

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("securefile: не удалось открыть файл для бинарной записи: " + path.string());
    }

    for (std::size_t index = 0; index < plainRecords.size(); ++index)
    {
        std::string encrypted = encryptBytes(wrapPlainRecord(plainRecords[index], 0));
        if (encrypted.size() > 0xFFFFFFFFu)
        {
            throw std::runtime_error("securefile: protobuf-запись слишком большая");
        }

        writeUint32(output, static_cast<std::uint32_t>(encrypted.size()));
        output.write(encrypted.data(), static_cast<std::streamsize>(encrypted.size()));
    }
}

// Добавление записи в конец
void appendSecureRecord(const std::filesystem::path& path, const std::string& plainRecord, std::streamoff* writtenOffset)
{
    ensureDirectoryExists(path.parent_path());

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output)
    {
        throw std::runtime_error("securefile: не удалось открыть файл для добавления protobuf-записи: " + path.string());
    }

    output.seekp(0, std::ios::end);
    if (writtenOffset != nullptr)
    {
        *writtenOffset = output.tellp();
    }

    std::string encrypted = encryptBytes(wrapPlainRecord(plainRecord, 0));
    if (encrypted.size() > 0xFFFFFFFFu)
    {
        throw std::runtime_error("securefile: protobuf-запись слишком большая");
    }

    writeUint32(output, static_cast<std::uint32_t>(encrypted.size()));
    output.write(encrypted.data(), static_cast<std::streamsize>(encrypted.size()));
}

// Чтение всего файла: длина - зашифрованные bytes - расшифровка
std::vector<std::string> readSecureRecords(const std::filesystem::path& path)
{
    std::vector<std::string> records;
    std::ifstream input(path, std::ios::binary);

    if (!input)
    {
        return records;
    }

    while (true)
    {
        std::uint32_t size = 0;
        if (!readUint32(input, size))
        {
            break;
        }

        std::string encrypted(size, '\0');
        input.read(&encrypted[0], static_cast<std::streamsize>(size));
        if (static_cast<std::uint32_t>(input.gcount()) != size)
        {
            throw std::runtime_error("securefile: повреждённый бинарный файл, запись обрезана");
        }

        records.push_back(unwrapPlainRecord(decryptBytes(encrypted)));
    }

    return records;
}

// Подобно прошлой, но с offset'ами
std::vector<std::pair<std::streamoff, std::string> > readSecureRecordsWithOffsets(const std::filesystem::path& path)
{
    std::vector<std::pair<std::streamoff, std::string> > records;
    std::ifstream input(path, std::ios::binary);

    if (!input)
    {
        return records;
    }

    while (true)
    {
        std::streamoff offset = input.tellg();
        std::uint32_t size = 0;
        if (!readUint32(input, size))
        {
            break;
        }

        std::string encrypted(size, '\0');
        input.read(&encrypted[0], static_cast<std::streamsize>(size));
        if (static_cast<std::uint32_t>(input.gcount()) != size)
        {
            throw std::runtime_error("securefile: повреждённый бинарный файл, запись обрезана");
        }

        records.push_back(std::make_pair(offset, unwrapPlainRecord(decryptBytes(encrypted))));
    }

    return records;
}


// Чтение одной записи
std::string readSecureRecordAtOffset(const std::filesystem::path& path, std::streamoff offset)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("securefile: не удалось открыть файл для чтения одной записи: " + path.string());
    }

    input.seekg(offset);

    std::uint32_t size = 0;
    if (!readUint32(input, size))
    {
        throw std::runtime_error("securefile: по указанному offset нет protobuf-записи");
    }

    std::string encrypted(size, '\0');
    input.read(&encrypted[0], static_cast<std::streamsize>(size));
    if (static_cast<std::uint32_t>(input.gcount()) != size)
    {
        throw std::runtime_error("securefile: protobuf-запись по offset обрезана");
    }

    return unwrapPlainRecord(decryptBytes(encrypted));
}


// Чтение заголовка для размера
std::streamoff secureRecordTotalSizeAtOffset(const std::filesystem::path& path, std::streamoff offset)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("securefile: не удалось открыть файл для чтения размера записи: " + path.string());
    }

    input.seekg(offset);

    std::uint32_t payloadSize = 0;
    if (!readUint32(input, payloadSize))
    {
        throw std::runtime_error("securefile: по указанному offset нет записи для чтения размера");
    }

    return static_cast<std::streamoff>(4 + payloadSize);
}

// Запись нового поверх tombstone'а
bool overwriteSecureRecordInSlot(
    const std::filesystem::path& path,
    const std::string& plainRecord,
    std::streamoff offset,
    std::streamoff slotTotalSize
)
{
    if (slotTotalSize < 4)
    {
        return false;
    }

    const std::size_t payloadCapacity = static_cast<std::size_t>(slotTotalSize - 4);
    if (payloadCapacity > 0xFFFFFFFFu)
    {
        return false;
    }

    std::string wrapped;
    try
    {
        wrapped = wrapPlainRecord(plainRecord, payloadCapacity);
    }
    catch (const std::exception&)
    {
        return false;
    }

    std::string encrypted = encryptBytes(wrapped);
    if (encrypted.size() != payloadCapacity)
    {
        throw std::runtime_error("securefile: внутренний размер tombstone-слота изменился неожиданным образом");
    }

    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file)
    {
        throw std::runtime_error("securefile: не удалось открыть файл для переиспользования tombstone: " + path.string());
    }

    file.seekp(offset);
    writeUint32(file, static_cast<std::uint32_t>(payloadCapacity));
    file.write(encrypted.data(), static_cast<std::streamsize>(encrypted.size()));
    return true;
}

// Проверка наличия файла
bool secureFileExists(const std::filesystem::path& path)
{
    return std::filesystem::exists(path);
}

// Создание нового пустого файла
void createEmptySecureFile(const std::filesystem::path& path)
{
    ensureDirectoryExists(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output)
    {
        throw std::runtime_error("securefile: не удалось создать бинарный файл: " + path.string());
    }
}

// Переводчик
void writeSecureLines(const std::filesystem::path& path, const std::vector<std::string>& plainLines)
{
    writeSecureRecords(path, plainLines);
}

void appendSecureLine(const std::filesystem::path& path, const std::string& plainLine, std::streamoff* writtenOffset)
{
    appendSecureRecord(path, plainLine, writtenOffset);
}

std::vector<std::string> readSecureLines(const std::filesystem::path& path)
{
    return readSecureRecords(path);
}

std::vector<std::pair<std::streamoff, std::string> > readSecureLinesWithOffsets(const std::filesystem::path& path)
{
    return readSecureRecordsWithOffsets(path);
}

