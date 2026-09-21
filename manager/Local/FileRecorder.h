#ifndef EXTENSION_FILERECORDER_H
#define EXTENSION_FILERECORDER_H

#include <cstdio>
#include <memory>
#include <string>
#include <fstream>
#include "Util/File.h"
#include "Util/logger.h"
#include "json/json.h"

namespace managerkit {

template<typename Type, typename Helper>
class FileRecorder {
public:
    using Ptr = std::shared_ptr<FileRecorder<Type, Helper>>;

    FileRecorder(const std::string &file_path) : _file_path(file_path) {
        if (!toolkit::File::fileExist(file_path)) {
            // create_file() is called for its mkdir -p of the parent directories, but it also
            // fopen()s the file and hands back the handle. Dropping that handle leaked one fd per
            // record file for the whole life of the process (one per camera's info.txt, one per
            // speaker, one per audio file record). Nothing reads or writes through it: every access
            // below opens its own stream.
            if (auto fp = toolkit::File::create_file(file_path, "wb+")) {
                fclose(fp);
            }
            TraceL << "Created file record: " << file_path;
        }
    };

    bool empty() { return toolkit::File::fileSize(_file_path) == 0; }

    bool load(Type &data) { 
        std::string json_str = load_string();
        return Helper::getParams(json_str, data);
    }

    void save(Type data) {
        std::string json_str = Helper::getParamsString(data);
        return save_string(json_str);
    }

    void remove() { 
        toolkit::File::delete_file(_file_path, true);
        TraceL << "Removed file record success: " << _file_path;
    }

private:
    std::string load_string() {
        std::ifstream ifs(_file_path, std::ios::binary | std::ios::ate);
        if (!ifs) {
            throw std::runtime_error("Cannot open file for reading: " + _file_path);
        }
        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);

        std::string buffer(size, '\0');
        ifs.read(&buffer[0], size);
        ifs.close();
        TraceL << "Loaded file record success: " << _file_path;
        return buffer;
    }

    void save_string(const std::string &data) {
        std::ofstream ofs(_file_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            throw std::runtime_error("Cannot open file for writing: " + _file_path);
        }
        ofs.write(data.data(), data.size());
        ofs.close();
        TraceL << "Saved file record success: " << _file_path;
    }

private:
    std::string _file_path;
};

} // namespace managerkit

#endif // EXTENSION_FILERECORDER_H