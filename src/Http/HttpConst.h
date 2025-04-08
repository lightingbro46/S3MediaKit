#ifndef S3MEDIAKIT_HTTPCONST_H
#define S3MEDIAKIT_HTTPCONST_H

#include <string>

namespace mediakit{

class HttpConst {
public:
    HttpConst() = delete;
    ~HttpConst() = delete;

    /**
     * Get character description based on http error code
     * @param status For example 404
     * @return Error code character description, for example Not Found
     */
    static const char *getHttpStatusMessage(int status);

    /**
     * Return http mime based on file suffix
     * @param name File suffix, for example html
     * @return mime value, for example text/html
     */
    static const std::string &getHttpContentType(const char *name);
};

}//mediakit

#endif //S3MEDIAKIT_HTTPCONST_H
