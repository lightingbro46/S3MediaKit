#include "Certification.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Util/File.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

namespace Manager {
#define MANAGER_FIELD "manager."
const std::string kCertSavePath = MANAGER_FIELD"certSavePath";

static onceToken token([]() {
    mINI::Instance()[kCertSavePath] = "./certs";
});
} // namespace Manager

CertificateImp::CertificateImp(const std::string &path) : _save_path(path) {
    if (_save_path.empty()) {
        GET_CONFIG(string, cert_save_path, Manager::kCertSavePath)
        _save_path = File::absolutePath("",  cert_save_path);    
    }
}

bool CertificateImp::certExist(const std::string &filename, const string &pem) {
    bool isExist = false;
    auto fullname = filename;
    if (!end_with(fullname, ".pem")) {
        fullname += ".pem";
    }
    auto file_path = File::absolutePath(fullname, _save_path);
    File::scanDir(_save_path, [&](const string &path, bool isDir) {
        if (!isDir && path == file_path) {
            auto content = File::loadFile(path);
            if (content == pem) {
                isExist = true;
                return false;
            }
        }
        return true;
    });
    return isExist;
}

void CertificateImp::saveCert(const std::string &filename, const string &pem) {
    auto fullname = filename;
    if (!end_with(fullname, ".pem")) {
        fullname += ".pem";
    }
    auto file_path = File::absolutePath(fullname, _save_path);
    if (File::fileExist(file_path)) {
        File::delete_file(file_path);
        DebugL << "Removed old file cert: " << fullname;
    }

    File::create_file(file_path, "wb");
    if (!File::saveFile(pem, file_path)) {
        throw std::runtime_error(string("Failed to write the file cert:") + file_path);
    }
    DebugL << "Saved file cert: " << fullname;
}

} // namespace managerkit
