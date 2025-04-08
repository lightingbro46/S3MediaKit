#ifndef SRC_SHELL_SHELLSESSION_H_
#define SRC_SHELL_SHELLSESSION_H_

#include <functional>
#include "Util/TimeTicker.h"
#include "Network/Session.h"

namespace mediakit {

class ShellSession: public toolkit::Session {
public:
    ShellSession(const toolkit::Socket::Ptr &_sock);

    void onRecv(const toolkit::Buffer::Ptr &) override;
    void onError(const toolkit::SockException &err) override;
    void onManager() override;

private:
    inline bool onCommandLine(const std::string &);
    inline void pleaseInputUser();
    inline void pleaseInputPasswd();
    inline void printShellPrefix();

    std::function<bool(const std::string &)> _loginInterceptor;
    std::string _strRecvBuf;
    toolkit::Ticker _beatTicker;
    std::string _strUserName;
};

} /* namespace mediakit */

#endif /* SRC_SHELL_SHELLSESSION_H_ */
