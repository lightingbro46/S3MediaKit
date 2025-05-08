file(GLOB MANAGER_SRC_LIST *.cpp *.h *.hpp)

# === Bổ sung: xử lý protobuf ===
# Yêu cầu protobuf
find_package(Protobuf REQUIRED)


# Đường dẫn thư mục chứa các file .proto
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)

# Tìm các file .proto
file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

# Generate pb.cc và pb.h từ .proto
protobuf_generate_cpp(
  PROTO_SRCS 
  PROTO_HDRS
  ${PROTO_FILES}
)

# Thêm các file sinh ra vào danh sách build
list(APPEND MANAGER_SRC_LIST ${PROTO_SRCS} ${PROTO_HDRS})

# === Kết thúc phần protobuf ===

add_library(s3managerkit STATIC ${MANAGER_SRC_LIST})
add_library(S3MediaKit::ManagerKit ALIAS s3managerkit)
target_link_libraries(s3managerkit
  PUBLIC
    S3MediaKit::MediaKit
    S3MediaKit::ToolKit)
target_compile_options(s3managerkit
  PRIVATE ${COMPILE_OPTIONS_DEFAULT})
target_include_directories(s3managerkit
  PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")

update_cached_list(MK_COMPILE_DEFINITIONS ENABLE_MANAGER)
update_cached_list(MK_LINK_LIBRARIES S3MediaKit::ManagerKit)

message(STATUS "managerkit function is enabled")