cmake_minimum_required(VERSION 3.20)
project(cutefish-messages VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CutefishToolkit REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(QRENCODE REQUIRED libqrencode)
pkg_check_modules(SQLITE REQUIRED sqlite3)

add_executable(cutefish-messages
    src/main.cpp
    src/messages_client.cpp
    src/notifier.cpp
    src/sms_backend.cpp
    src/signal_rpc.cpp
    src/signal_backend.cpp
    src/signal_store.cpp
    src/avatar_cache.cpp
    src/messages_window.cpp
    src/phone/discovery.cpp
    src/phone/certificate.cpp
    src/phone/link.cpp
)
target_include_directories(cutefish-messages PRIVATE src ${QRENCODE_INCLUDE_DIRS} ${SQLITE_INCLUDE_DIRS})
target_link_libraries(cutefish-messages PRIVATE Cutefish::Toolkit OpenSSL::SSL OpenSSL::Crypto ${QRENCODE_LIBRARIES} ${SQLITE_LIBRARIES})

add_executable(discovery-probe src/tools/discovery_probe.cpp src/phone/discovery.cpp)
target_include_directories(discovery-probe PRIVATE src)
target_link_libraries(discovery-probe PRIVATE Cutefish::ToolkitBase)

add_executable(store-probe src/tools/store_probe.cpp src/signal_store.cpp)
target_include_directories(store-probe PRIVATE src ${SQLITE_INCLUDE_DIRS})
target_link_libraries(store-probe PRIVATE Cutefish::ToolkitBase ${SQLITE_LIBRARIES})

add_executable(pair-probe src/tools/pair_probe.cpp
    src/phone/discovery.cpp src/phone/certificate.cpp src/phone/link.cpp
    src/avatar_cache.cpp)
target_include_directories(pair-probe PRIVATE src)
target_link_libraries(pair-probe PRIVATE Cutefish::ToolkitBase OpenSSL::SSL OpenSSL::Crypto)

install(TARGETS cutefish-messages RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/cutefish-messages.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/icons/org.cutefish.Messages.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
cutefish_add_translations(cutefish-messages SOURCES src DESKTOP_FILES data/cutefish-messages.desktop)
