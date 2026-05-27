#pragma once

#include "types.h"
#include "course_storage.pb.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class IndexBase
{
public:
    virtual ~IndexBase() {}

    virtual void clear() = 0;
    virtual bool insert(const Value& key, std::streamoff offset) = 0;
    virtual bool erase(const Value& key) = 0;

    virtual std::string exportTreeToProtoBytes() const = 0;
    virtual void importTreeFromProtoBytes(const std::string& bytes) = 0;

    virtual bool contains(const Value& key) const = 0;
    virtual std::optional<std::streamoff> find(const Value& key) const = 0;
    virtual std::vector<std::streamoff> lessThan(const Value& key, bool inclusive) const = 0;
    virtual std::vector<std::streamoff> greaterThan(const Value& key, bool inclusive) const = 0;
    virtual std::vector<std::streamoff> between(const Value& low, const Value& high) const = 0;
};

class DiskBStarIndex : public IndexBase
{
public:
    DiskBStarIndex(
        const std::filesystem::path& pageFilePath,
        const std::filesystem::path& metaFilePath,
        ColumnType keyType
    );

    void clear() override;
    bool insert(const Value& key, std::streamoff offset) override;
    bool erase(const Value& key) override;

    std::string exportTreeToProtoBytes() const override;
    void importTreeFromProtoBytes(const std::string& bytes) override;

    bool contains(const Value& key) const override;
    std::optional<std::streamoff> find(const Value& key) const override;
    std::vector<std::streamoff> lessThan(const Value& key, bool inclusive) const override;
    std::vector<std::streamoff> greaterThan(const Value& key, bool inclusive) const override;
    std::vector<std::streamoff> between(const Value& low, const Value& high) const override;

private:
    struct PageLocation
    {
        std::streamoff offset = 0;
        std::streamoff size = 0;
    };

    struct FreeSlot
    {
        std::streamoff offset = 0;
        std::streamoff size = 0;
    };

    struct HeaderData
    {
        bool rootExists = false;
        long long rootPageId = -1;
        long long nextPageId = 0;
        long long size = 0;
        std::map<long long, PageLocation> locations;
        std::vector<FreeSlot> freeSlots;
    };

    std::filesystem::path pageFilePath_;
    std::filesystem::path metaFilePath_;
    ColumnType keyType_;

    static const int kMinDegree = 16;
    static const int kMaxEntries = 2 * kMinDegree - 1;
    static const int kMinEntries = kMinDegree - 1;

    HeaderData loadHeader() const;
    void saveHeader(const HeaderData& header) const;
    void ensureFilesExist() const;

    ProtoBStarPage makePage(HeaderData& header, bool isLeaf) const;
    ProtoBStarPage readPage(const HeaderData& header, long long pageId) const;
    void writePage(HeaderData& header, const ProtoBStarPage& page) const;
    void freeOldPageSlot(HeaderData& header, long long pageId) const;

    Value entryKey(const ProtoIndexEntry& entry) const;
    ProtoIndexEntry makeEntry(const Value& key, std::streamoff offset, bool deleted) const;
    int compareKeyWithEntry(const Value& key, const ProtoIndexEntry& entry) const;
    int findEntryPosition(const ProtoBStarPage& page, const Value& key) const;

    std::vector<ProtoIndexEntry> pageEntries(const ProtoBStarPage& page) const;
    std::vector<long long> pageChildren(const ProtoBStarPage& page) const;
    void replaceEntries(ProtoBStarPage& page, const std::vector<ProtoIndexEntry>& entries) const;
    void replaceChildren(ProtoBStarPage& page, const std::vector<long long>& children) const;
    void compactLeafTombstones(ProtoBStarPage& page) const;

    bool insertNonFull(HeaderData& header, long long pageId, const Value& key, std::streamoff offset);

    // For вставка
    bool prepareChildForInsert(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    bool redistributeWithLeftForInsert(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    bool redistributeWithRightForInsert(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    bool splitTwoToThreeWithLeft(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    bool splitTwoToThreeWithRight(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    void splitChild(HeaderData& header, ProtoBStarPage& parent, int childIndex);

    // For удаление
    bool eraseInPage(HeaderData& header, long long pageId, const Value& key);
    void repairChildAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    bool borrowFromLeftAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    bool borrowFromRightAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    bool mergeThreeToTwoAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    bool mergeTwoToOneAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex);
    void fixRootAfterErase(HeaderData& header);
    std::optional<std::streamoff> findInPage(const HeaderData& header, long long pageId, const Value& key) const;

    void collectInOrder(const HeaderData& header, long long pageId, std::vector<ProtoIndexEntry>& entries) const;
    std::vector<std::streamoff> filterOffsets(const std::vector<ProtoIndexEntry>& entries, const Value& low, bool hasLow, const Value& high, bool hasHigh, bool includeLow, bool includeHigh) const;
};
