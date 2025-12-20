#ifndef SRC_SHELL_SHELLCMD_H_
#define SRC_SHELL_SHELLCMD_H_

#include "Util/CMD.h"
#include "Common/MediaSource.h"

namespace mediakit {

class CMD_media : public toolkit::CMD {
public:
    CMD_media() {
        _parser.reset(new toolkit::OptionParser([](const std::shared_ptr<std::ostream> &stream, toolkit::mINI &ini) {
            MediaSource::for_each_media([&](const MediaSource::Ptr &media) {
                if (ini.find("list") != ini.end()) {
                    // List sources
                    (*stream) << "\t" << media->getUrl() << "\r\n";
                    return;
                }

                toolkit::EventPollerPool::Instance().getPoller()->async([ini, media, stream]() {
                    if (ini.find("kick") != ini.end()) {
                        // Kick out sources
                        do {
                            if (!media) {
                                break;
                            }
                            media->getOwnerPoller()->async([media]() {
                                media->close(true);
                            });
                            (*stream) << "\tKicked out successfully:" << media->getUrl() << "\r\n";
                            return;
                        } while (0);
                        (*stream) << "\tKicked out failed:" << media->getUrl() << "\r\n";
                    }
                }, false);


            }, ini["schema"], ini["vhost"], ini["app"], ini["stream"]);
        }));
        (*_parser) << toolkit::Option('k', "kick", toolkit::Option::ArgNone, nullptr, false, "Kick out media sources", nullptr);
        (*_parser) << toolkit::Option('l', "list", toolkit::Option::ArgNone, nullptr, false, "List media sources", nullptr);
        (*_parser) << toolkit::Option('S', "schema", toolkit::Option::ArgRequired, nullptr, false, "Protocol filtering", nullptr);
        (*_parser) << toolkit::Option('v', "vhost", toolkit::Option::ArgRequired, nullptr, false, "Virtual Host Filter", nullptr);
        (*_parser) << toolkit::Option('a', "app", toolkit::Option::ArgRequired, nullptr, false, "Application name filter", nullptr);
        (*_parser) << toolkit::Option('s', "stream", toolkit::Option::ArgRequired, nullptr, false, "Flow id filtering", nullptr);
    }

    const char *description() const override {
        return "Media source-related operations.";
    }
};

} /* namespace mediakit */

#endif //SRC_SHELL_SHELLCMD_H_