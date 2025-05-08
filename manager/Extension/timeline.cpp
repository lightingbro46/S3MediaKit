#include "timeline.h"

void install() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
}

void uninstall() {
    google::protobuf::ShutdownProtobufLibrary();
}

void write_block(std::ofstream &out, const TimeBlock &block) {
    std::string data;
    block.SerializeToString(&data);
    uint32_t size = data.size();
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    out.write(data.data(), size);
}

void read_block(std::ifstream &in) {
    while (in.peek() != EOF) {
        uint32_t size;
        in.read(reinterpret_cast<char*>(&size), sizeof(size));
        if (in.gcount() != sizeof(size)) break;

        std::string data(size, '\0');
        in.read(&data[0], size);

        TimeBlock block;
        if (block.ParseFromString(data)) {
            std::cout << "App: " << block.app()
                      << ", ID: " << block.id()
                      << ", Start time: " << block.start_time()
                      << ", Time len: " << block.time_len()
                      << ", File path: " << block.file_path() << std::endl;
        } else {
            std::cerr << "Lỗi khi parse Person." << std::endl;
        }
    }
}