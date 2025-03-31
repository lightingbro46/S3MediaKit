/*
 * Copyright (c) 2025-present The S3MediaKit project authors. All Rights Reserved.
 *
 * This file is part of S3MediaKit(https://github.com/S3MediaKit/S3MediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <memory.h>
#include <set>
#include <deque>
#include "Util/CMD.h"
#include "Util/util.h"
#include "Util/logger.h"
#include "Util/File.h"

using namespace std;
using namespace toolkit;

class CMD_main : public CMD {
public:
    CMD_main() {
        _parser.reset(new OptionParser(nullptr));
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
};

vector<string> split(const string& s, const char *delim) {
    vector<string> ret;
    size_t last = 0;
    auto index = s.find(delim, last);
    while (index != string::npos) {
        if (index - last >= 0) {
            ret.push_back(s.substr(last, index - last));
        }
        last = index + strlen(delim);
        index = s.find(delim, last);
    }
    if (!s.size() || s.size() - last >= 0) {
        ret.push_back(s.substr(last));
    }
    return ret;
}

void process_file(const char *file) {
    auto str = File::loadFile(file);
    if (str.empty()) {
        return;
    }
    auto lines = ::split(str, "\n");
    deque<string> lines_copy;
    for (auto &line : lines) {
        if(line.empty()){
            lines_copy.push_back("");
            continue;
        }
        string line_copy;
        bool flag = false;
        int i = 0;
        for (auto &ch : line) {
            ++i;
            switch (ch) {
                case '\t' :
                    line_copy.append("    ");
                    break;
                case ' ':
                    line_copy.push_back(ch);
                    break;
                default:
                    line_copy.push_back(ch);
                    flag = true;
                    break;
            }
            if (flag) {
                line_copy.append(line.substr(i));
                break;
            }
        }
        lines_copy.push_back(line_copy);
    }
    str.clear();
    for (auto &line : lines_copy) {
        str.append(line);
        str.push_back('\n');
    }
    if(!lines_copy.empty()){
        str.pop_back();
    }
    File::saveFile(str, file);
}

// / This program is for unified replacement of tabs with 4 spaces
int main(int argc, char *argv[]) {
    CMD_main cmd_main;
    try {
        cmd_main.operator()(argc, argv);
    } catch (std::exception &ex) {
        cout << ex.what() << endl;
        return -1;
    }

    string path = cmd_main["in"];
    string filter = cmd_main["filter"];
    auto vec = ::split(filter, ",");

    set<string> filter_set;
    for (auto ext : vec) {
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
        process_file(path.data());
        return true;
    }, true);
    return 0;
}
