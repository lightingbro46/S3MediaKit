#include <stdlib.h>
#include <memory.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif //!defined(_WIN32)
#include <set>
#include "Util/CMD.h"
#include "Util/util.h"
#include "Util/logger.h"
#include "Util/File.h"
#include "Util/uv_errno.h"

using namespace std;
using namespace toolkit;

class CMD_main : public CMD {
public:
    CMD_main() {
        _parser.reset(new OptionParser(nullptr));


        (*_parser) << Option('r',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "rm",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgNone,/*This option must be followed by a value*/
                             nullptr,/*This option default value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Whether to delete or add a bom, the default bom header is added",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('f',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "filter",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "c,cpp,cxx,c,h,hpp",/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "File suffix filter",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('i',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "in",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             nullptr,/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Folders or files",/*This option specifies text*/
                             nullptr);
    }

    virtual ~CMD_main() {}

    virtual const char *description() const {
        return "Add or delete a bom";
    }
};

static const char s_bom[] = "\xEF\xBB\xBF";

void add_or_rm_bom(const char *file,bool rm_bom){
    auto file_str = File::loadFile(file);
    if(rm_bom){
        file_str.erase(0, sizeof(s_bom) - 1);
    }else{
        file_str.insert(0,s_bom,sizeof(s_bom) - 1);
    }
    File::saveFile(file_str,file);
}

void process_file(const char *file,bool rm_bom){
    std::shared_ptr<FILE> fp(fopen(file, "rb+"), [](FILE *fp) {
        if (fp) {
            fclose(fp);
        }
    });

    if (!fp) {
        WarnL << "Failed to open the file:" << file << " " << get_uv_errmsg();
        return;
    }

    bool have_bom = rm_bom;
    char buf[sizeof(s_bom) - 1] = {0};

    if (sizeof(buf) == fread(buf,1,sizeof(buf),fp.get())) {
        have_bom = (memcmp(s_bom, buf, sizeof(s_bom) - 1) == 0);
    }

    if (have_bom == !rm_bom) {
    // DebugL << "No need to" << (rm_bom ? "remove" : "add") << "bom:" << file;
        return;
    }

    fp = nullptr;
    add_or_rm_bom(file,rm_bom);
    InfoL << (rm_bom ? "Delete" : "Add") << "bom:" << file;
}

// / This program is for unified adding or removing utf-8 bom header
int main(int argc, char *argv[]) {
    CMD_main cmd_main;
    try {
        cmd_main.operator()(argc, argv);
    } catch (std::exception &ex) {
        cout << ex.what() << endl;
        return -1;
    }

    bool rm_bom = cmd_main.hasKey("rm");
    string path = cmd_main["in"];
    string filter = cmd_main["filter"];
    auto vec = split(filter,",");

    set<string> filter_set;
    for(auto ext : vec){
        filter_set.emplace(ext);
    }

    bool no_filter = filter_set.find("*") != filter_set.end();
    // Set log
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    File::scanDir(path, [&](const string &path, bool isDir) {
        if (isDir) {
            return true;
        }
        if (!no_filter) {
            // Filter enabled
            auto pos = strstr(path.data(), ".");
            if (pos == nullptr) {
                // No suffix
                return true;
            }
            auto ext = pos + 1;
            if (filter_set.find(ext) == filter_set.end()) {
                // Suffix does not match
                return true;
            }
        }
        // File matches
        process_file(path.data(), rm_bom);
        return true;
    }, true);
    return 0;
}
