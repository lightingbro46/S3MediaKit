#include "Certification.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Util/File.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

namespace Manager {
#define GENERAL_FIELD "manager."
const std::string kCertSavePath = GENERAL_FIELD"certSavePath";

static onceToken token([]() {
    mINI::Instance()[kCertSavePath] = "./certs";
});
} // namespace Manager

CertificateImp::CertificateImp(const std::string &path) : _save_path(path) {
    if (_save_path.empty()) {
        GET_CONFIG(string, cert_save_path, Manager::kCertSavePath)
        _save_path = File::absolutePath("",  cert_save_path);    
    }

    File::scanDir(_save_path, [&](const string &path, bool isDir) {
        if (!isDir && end_with(path, ".pem")) {
            auto filename = findSubString(path.data() + _save_path.size() + 1, nullptr, ".pem");
            int file_index = std::stoi(filename);
            if (file_index > _last_index) {
                _last_index = file_index;
            }
        }
        return true;
    });
}

bool CertificateImp::certExist(const string &pem) {
    bool isExist = false;
    File::scanDir(_save_path, [&](const string &path, bool isDir) {
        if (!isDir && end_with(path, ".pem")) {
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

void CertificateImp::saveCert(const string &pem) {
    string filename = to_string(++_last_index) + ".pem";
    auto file_path = File::absolutePath(filename, _save_path);
    File::create_file(file_path, "wb");
    if (!File::saveFile(pem, file_path)) {
        throw std::runtime_error(string("Failed to write the file:") + file_path);
    }
    DebugL << "Saved file cert: " << filename;
}

} // namespace managerkit
