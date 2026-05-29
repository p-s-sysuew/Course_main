cmake_minimum_required(VERSION 3.16)
project(course_work_coursework LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)

# Указываем путь к заголовочным файлам Crow напрямую
set(CROW_INCLUDE_DIR "$ENV{HOME}/Загрузки/Crow/include")

# Генерация файлов Protobuf
protobuf_generate_cpp(PROTO_SOURCES PROTO_HEADERS course_storage.proto)

# Исходные файлы проекта
set(COMMON_SOURCES
    src/utils.cpp
    src/securefile.cpp
    src/stringpool.cpp
    src/lexer.cpp
    src/parser.cpp
    src/storage.cpp
    src/jsonhelper.cpp
    src/table.cpp
    src/diskbstarindex.cpp
    src/dbms.cpp
    src/logger.cpp
    src/runner.cpp
    src/tablelocks.cpp
    src/auth.cpp
    src/telemetry.cpp
    src/heartbeat.cpp
    ${PROTO_SOURCES}
)

# Главные исполняемые файлы 
add_executable(course_work src/main.cpp ${COMMON_SOURCES})
add_executable(course_client src/client.cpp src/utils.cpp)

add_executable(course_logs
    src/logviewer.cpp
    src/securefile.cpp
    src/utils.cpp
    ${PROTO_SOURCES}
)

# Сервер СУБД
add_executable(course_server src/server.cpp ${COMMON_SOURCES})
target_include_directories(course_server PRIVATE include . ${CMAKE_CURRENT_BINARY_DIR} ${CROW_INCLUDE_DIR})
target_link_libraries(course_server PRIVATE protobuf::libprotobuf OpenSSL::Crypto pthread)

# Настраиваем пути включения (include) для остальных таргетов
target_include_directories(course_work PRIVATE include . ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(course_client PRIVATE include .)
target_include_directories(course_logs PRIVATE include . ${CMAKE_CURRENT_BINARY_DIR})

# Ссылки на библиотеки
target_link_libraries(course_work PRIVATE protobuf::libprotobuf OpenSSL::Crypto pthread)
target_link_libraries(course_logs PRIVATE protobuf::libprotobuf pthread)

# Флаги компилятора (ошибка была здесь, теперь всё исправлено)
if (MSVC)
    target_compile_options(course_work PRIVATE /W4)
    target_compile_options(course_client PRIVATE /W4)
    target_compile_options(course_logs PRIVATE /W4)
    target_compile_options(course_server PRIVATE /W4)
else()
    target_compile_options(course_work PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(course_client PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(course_logs PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(course_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Тесты
if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bstarindex.cpp")
    add_executable(test_bstarindex tests/test_bstarindex.cpp)
    target_include_directories(test_bstarindex PRIVATE include .)
    add_test(NAME bstarindex COMMAND test_bstarindex)
endif()