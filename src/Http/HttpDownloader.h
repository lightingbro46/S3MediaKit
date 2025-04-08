#ifndef SRC_HTTP_HTTPDOWNLOADER_H_
#define SRC_HTTP_HTTPDOWNLOADER_H_

#include "HttpClientImp.h"

namespace mediakit {

class HttpDownloader : public HttpClientImp {
public:
    using Ptr = std::shared_ptr<HttpDownloader>;
    using onDownloadResult = std::function<void(const toolkit::SockException &ex, const std::string &filePath)>;

    ~HttpDownloader() override;

    /**
     * Start downloading the file, default to resume download
     * @param url Download http url
     * @param file_path File save address, leave blank to choose the default file path
     * @param append If the file already exists, whether to download in resume mode
     */
    void startDownload(const std::string &url, const std::string &file_path = "", bool append = false);

    void startDownload(const std::string &url, const onDownloadResult &cb) {
        setOnResult(cb);
        startDownload(url, "", false);
    }

    void setOnResult(const onDownloadResult &cb) { _on_result = cb; }

protected:
    void onResponseBody(const char *buf, size_t size) override;
    void onResponseHeader(const std::string &status, const HttpHeader &headers) override;
    void onResponseCompleted(const toolkit::SockException &ex) override;

private:
    void closeFile();

private:
    FILE *_save_file = nullptr;
    std::string _file_path;
    onDownloadResult _on_result;
};

} /* namespace mediakit */

#endif /* SRC_HTTP_HTTPDOWNLOADER_H_ */
