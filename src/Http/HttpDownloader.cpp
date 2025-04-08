#include "HttpDownloader.h"
#include "Util/File.h"
#include "Util/MD5.h"
using namespace toolkit;
using namespace std;

namespace mediakit {

HttpDownloader::~HttpDownloader() {
    closeFile();
}

void HttpDownloader::startDownload(const string &url, const string &file_path, bool append) {
    _file_path = file_path;
    if (_file_path.empty()) {
        _file_path = exeDir() + "HttpDownloader/" + MD5(url).hexdigest();
    }
    _save_file = File::create_file(_file_path, append ? "ab" : "wb");
    if (!_save_file) {
        auto strErr = StrPrinter << "Failed to open the file:" << file_path << endl;
        throw std::runtime_error(strErr);
    }
    if (append) {
        auto currentLen = ftell(_save_file);
        if (currentLen) {
            // Resume downloading at least one byte to avoid encountering a http 416 error
            currentLen -= 1;
            fseek(_save_file, -1, SEEK_CUR);
        }
        addHeader("Range", StrPrinter << "bytes=" << currentLen << "-" << endl);
    }
    setMethod("GET");
    sendRequest(url);
}

void HttpDownloader::onResponseHeader(const string &status, const HttpHeader &headers) {
    if (status != "200" && status != "206") {
        // Failure
        throw std::invalid_argument("bad http status: " + status);
    }
}

void HttpDownloader::onResponseBody(const char *buf, size_t size) {
    if (_save_file) {
        fwrite(buf, size, 1, _save_file);
    }
}

void HttpDownloader::onResponseCompleted(const SockException &ex) {
    closeFile();
    if (_on_result) {
        _on_result(ex, _file_path);
        _on_result = nullptr;
    }
}

void HttpDownloader::closeFile() {
    if (_save_file) {
        fflush(_save_file);
        fclose(_save_file);
        _save_file = nullptr;
    }
}

} /* namespace mediakit */
