#ifndef S3MANAGERKIT_CERTIFICATION_H
#define S3MANAGERKIT_CERTIFICATION_H

#include <string>
#include "DbStorage.h"

namespace managerkit {

struct Certificate {   
    int id;
    std::string type;
    std::string pem;
};

DECLARE_ENTITY(Certificate, "certificate",
    {"id"},
    &Certificate::id, "id", 
    &Certificate::type, "type", 
    &Certificate::pem, "pem"
)

class CertificateRepository : public SqliteRepository<Certificate> {
public:
    CertificateRepository() : SqliteRepository<Certificate>(Database::kMediaServerDb) {}
};

class CertificateImp {
public:
    using Ptr = std::shared_ptr<CertificateImp>;

    explicit CertificateImp(const std::string &path = "");
    ~CertificateImp() = default;

    bool certExist(const std::string &pem);

    void saveCert(const std::string &pem);

private:
    int _last_index = -1;
    std::string _save_path;
};
} // namespace managerkit

#endif // S3MANAGERKIT_CERTIFICATION_H