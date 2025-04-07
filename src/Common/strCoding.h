#ifndef SRC_HTTP_STRCODING_H_
#define SRC_HTTP_STRCODING_H_

#include <iostream>
#include <string>
#include <cstdint>
namespace mediakit {

class strCoding {
public:
    static std::string UrlEncodePath(const std::string &str); //url path utf8 encoding
    static std::string UrlEncodeComponent(const std::string &str); // url parameter utf8 encoding
    static std::string UrlDecodePath(const std::string &str); //url path utf8 decoding
    static std::string UrlDecodeComponent(const std::string &str); // url parameter utf8 decoding
    static std::string UrlEncodeUserOrPass(const std::string &str); // Username and password encoding in url
    static std::string UrlDecodeUserOrPass(const std::string &str); // Decode username and password in url
#if defined(_WIN32)
    static std::string UTF8ToGB2312(const std::string &str);//utf_8 converts to gb2312
    static std::string GB2312ToUTF8(const std::string &str); //gb2312 to utf_8
#endif//defined(_WIN32)
private:
    strCoding(void);
    virtual ~strCoding(void);
};

} /* namespace mediakit */

#endif /* SRC_HTTP_STRCODING_H_ */
