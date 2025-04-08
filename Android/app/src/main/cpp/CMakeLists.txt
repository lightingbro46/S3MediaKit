project(S3MediaKit)
cmake_minimum_required(VERSION 3.1.3)
#Enable c++11
set(CMAKE_CXX_STANDARD 11)

#Set the path to the last output of the generated so dynamic library
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libs_export/${ANDROID_ABI})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libs_export/${ANDROID_ABI})
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/libs_export/${ANDROID_ABI}/binary)
set(OPENSSL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/${ANDROID_ABI}")

#Because the openssl library does not specify -fPIC when compiling, the github action ci fails to compile, block it first
#set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
#set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/libcrypto.a")
#set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/libssl.a")

#Set the project source code root directory
set(JNI_Root ${CMAKE_CURRENT_SOURCE_DIR})
set(S3MediaKit_Root ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../)

#Add the main project cmake
add_subdirectory(${S3MediaKit_Root} ${EXECUTABLE_OUTPUT_PATH})

#Set include
include_directories(${JNI_Root})
include_directories(${S3MediaKit_Root}/src)
include_directories(${S3MediaKit_Root}/srt)
include_directories(${S3MediaKit_Root}/webrtc)
include_directories(${S3MediaKit_Root}/server)
include_directories(${S3MediaKit_Root}/3rdpart)
include_directories(${S3MediaKit_Root}/3rdpart/media-server)
include_directories(${S3MediaKit_Root}/3rdpart/ZLToolKit/src)

#Collect source code and add dynamic libraries
file(GLOB JNI_src_list ${JNI_Root}/*.cpp ${JNI_Root}/*.h)
add_library(s3mediakit_jni SHARED ${JNI_src_list})

#Link
target_link_libraries(s3mediakit_jni -Wl,--start-group log z ${MK_LINK_LIBRARIES} -Wl,--end-group)

