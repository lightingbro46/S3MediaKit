include(CheckCXXSourceCompiles)

# Tìm đường dẫn chứa file sqlite3.h
if(WIN32)
    find_path(SQLite3_INCLUDE_DIR sqlite3.h
        PATHS
        $ENV{SQLITE3_INCLUDE_DIR}
        $ENV{SQLite3_DIR}/include
        $ENV{ProgramFiles}/SQLite/include
        $ENV{SystemDrive}/SQLite/include
        $ENV{ProgramW6432}/SQLite/include
    )
else()
    find_path(SQLite3_INCLUDE_DIR sqlite3.h
        PATHS
        $ENV{SQLite3_INCLUDE_DIR}
        $ENV{SQLite3_DIR}/include
        /usr/include
        /usr/local/include
        /opt/sqlite/include
    )
endif()

# Tìm thư viện SQLite3
if(WIN32)
    set(SQLite3_LIB_PATHS
        $ENV{SQLite3_DIR}/lib
        $ENV{ProgramFiles}/SQLite/lib
        $ENV{ProgramW6432}/SQLite/lib
        $ENV{SystemDrive}/SQLite/lib
    )
    find_library(SQLite3_LIBRARIES NAMES sqlite3
        PATHS ${SQLite3_LIB_PATHS}
    )
else()
    set(SQLite3_LIB_PATHS
        $ENV{SQLite3_DIR}/lib
        /usr/lib
        /usr/local/lib
        /opt/sqlite/lib
    )
    find_library(SQLite3_LIBRARIES NAMES sqlite3
        PATHS ${SQLite3_LIB_PATHS}
    )
endif()

# Kiểm tra thư viện SQLite3 có hỗ trợ `sqlite3_enable_load_extension`
set(CMAKE_REQUIRED_INCLUDES ${SQLite3_INCLUDE_DIR})
set(CMAKE_REQUIRED_LIBRARIES ${SQLite3_LIBRARIES})
check_cxx_source_compiles("
#include <sqlite3.h>
int main() { sqlite3_enable_load_extension(0, 1); return 0; }
" HAVE_SQLite3_ENABLE_LOAD_EXTENSION)

# Xác nhận SQLite3 đã tìm thấy
if(SQLite3_INCLUDE_DIR AND SQLite3_LIBRARIES)
    set(SQLite3_FOUND TRUE)
    message(STATUS "Found SQLite3: ${SQLITE3_INCLUDE_DIR}, ${SQLITE3_LIBRARIES}")
else()
    set(SQLite3_FOUND FALSE)
    message(STATUS "SQLite3 not found.")
endif()

mark_as_advanced(SQLite3_INCLUDE_DIR SQLite3_LIBRARIES)
