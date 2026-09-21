#ifndef ZLMEDIAKIT_RECORDENCRYPTIONCONFIG_H
#define ZLMEDIAKIT_RECORDENCRYPTIONCONFIG_H

#include "DbStorage.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

struct RecordEncryptionConfig {
    std::string id;
    int mode = 0;
    std::string verifySalt;
    std::string verifyHash;
    std::string saltEnc;
    std::string saltHmac;
    int encryptOldVideo = 0;
    int64_t updatedAt = 0;
};

DECLARE_ENTITY(RecordEncryptionConfig, "record_encryption_configs",
    { "id" },
    &RecordEncryptionConfig::id, "id",
    &RecordEncryptionConfig::mode, "mode",
    &RecordEncryptionConfig::verifySalt, "verifySalt",
    &RecordEncryptionConfig::verifyHash, "verifyHash",
    &RecordEncryptionConfig::saltEnc, "saltEnc",
    &RecordEncryptionConfig::saltHmac, "saltHmac",
    &RecordEncryptionConfig::encryptOldVideo, "encryptOldVideo",
    &RecordEncryptionConfig::updatedAt, "updatedAt"
)

class RecordEncryptionConfigRepository : public SqliteRepository<RecordEncryptionConfig> {
public:
    RecordEncryptionConfigRepository() : SqliteRepository<RecordEncryptionConfig>(Database::kMediaServerDb) {}

    std::vector<RecordEncryptionConfig> findAll() {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<RecordEncryptionConfig>::getColumns())
                .from(EntityTraits<RecordEncryptionConfig>::tableName()));
        std::vector<RecordEncryptionConfig> ret;
        for (const auto &row : rows)
            ret.push_back(EntityTraits<RecordEncryptionConfig>::fromRow(row));
        return ret;
    }
};

class RecordEncryptionConfigImp : public RecordEncryptionConfigRepository {
public:
    using Ptr = std::shared_ptr<RecordEncryptionConfigImp>;
    RecordEncryptionConfigImp() : RecordEncryptionConfigRepository() {}

    bool addOrUpdate(const RecordEncryptionConfig& config) {
        if (!findById(config.id).empty()) {
            updateById(config);
            return true;
        } else {
            return save(config, true);
        }
    }

    void remove(const RecordEncryptionConfig& config) {
        removeById(config);
    }

    std::vector<RecordEncryptionConfig> findById(const std::string &id) {
        RecordEncryptionConfig config;
        config.id = id;
        return RecordEncryptionConfigRepository::findById(config);
    }
};

} // namespace managerkit

#endif // ZLMEDIAKIT_RECORDENCRYPTIONCONFIG_H