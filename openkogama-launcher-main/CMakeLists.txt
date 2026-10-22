cmake_minimum_required(VERSION 3.19)
project(KoGaMaZ LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core CorePrivate Widgets Network Concurrent)

qt_standard_project_setup()

qt_add_executable(KoGaMaZ
    WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/mainwindow.ui
    resources/app.rc
    src/installdialog.h src/installdialog.cpp src/installdialog.ui
    src/installprogressdialog.h src/installprogressdialog.cpp src/installprogressdialog.ui
    src/launchdialog.h src/launchdialog.cpp src/launchdialog.ui
    src/assetinstaller.h src/assetinstaller.cpp
    src/worldsdialog.h src/worldsdialog.cpp src/worldsdialog.ui
    src/gamelauncher.h src/gamelauncher.cpp
    src/playerprofile.h src/playerprofile.cpp
    src/discordpresence.h src/discordpresence.cpp
    src/links.h src/links.cpp
    src/serverspage.h src/serverspage.cpp
    src/tunnel.h src/tunnel.cpp
    src/sharebar.h src/sharebar.cpp
    src/serverconfigdialog.h src/serverconfigdialog.cpp
    src/admindialog.h src/admindialog.cpp
    src/tilegrid.h src/tilegrid.cpp
    src/modelpreview.h src/modelpreview.cpp
    src/webplayerruntime.h src/webplayerruntime.cpp
    src/paths.h src/paths.cpp
    src/updater.h src/updater.cpp
    src/appversion.h
    src/version.h src/version.cpp
    src/updatecheck.h src/updatecheck.cpp
    src/theme.h src/theme.cpp
    src/settings.h src/settings.cpp
    src/social.h src/social.cpp
    src/localsocialserver.h src/localsocialserver.cpp
    src/brandlogo.h src/brandlogo.cpp
    src/playerstats.h src/playerstats.cpp
    src/profilepage.h src/profilepage.cpp
    src/friendsdialog.h src/friendsdialog.cpp
    src/chatdialog.h src/chatdialog.cpp
    src/worldmembersdialog.h src/worldmembersdialog.cpp
    src/settingsdialog.h src/settingsdialog.cpp src/settingsdialog.ui
    src/consolewindow.h src/consolewindow.cpp
    src/centereddelegate.h
    src/creditsdialog.h src/creditsdialog.cpp
)

qt_add_resources(KoGaMaZ "resources"
    PREFIX "/"
    BASE resources
    FILES
        resources/versions.json
        resources/discord.png
        resources/install.png
        resources/unknown.png
        resources/launch.png
        resources/lemonplus.png
        resources/logo.png
        resources/openkogama-font.ttf
        resources/launcher.qss
        resources/arrow-down.svg
        resources/check.svg
        resources/plus.svg
        resources/plus-light.svg
        resources/arrow-down-light.svg
)

include(FetchContent)
FetchContent_Declare(velopack
    URL https://github.com/velopack/velopack/releases/download/1.2.161/velopack_libc_1.2.161.zip
    URL_HASH SHA256=31de8f2cf05a47bd5328a9f98b0c41aec1582156f6075f1058b26941f7f17c20
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(velopack)
configure_file(${velopack_SOURCE_DIR}/lib/velopack_libc_win_x64_msvc.dll ${CMAKE_CURRENT_BINARY_DIR}/velopack_libc.dll COPYONLY)

add_library(velopack SHARED IMPORTED)
set_target_properties(velopack PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/velopack_libc.dll
    IMPORTED_IMPLIB ${velopack_SOURCE_DIR}/lib/velopack_libc_win_x64_msvc.dll.lib
    INTERFACE_INCLUDE_DIRECTORIES ${velopack_SOURCE_DIR}/include
)

target_link_libraries(KoGaMaZ
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
        Qt::CorePrivate
        Qt::Concurrent
        velopack
)

add_custom_command(TARGET KoGaMaZ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:velopack> $<TARGET_FILE_DIR:KoGaMaZ>
)

install(TARGETS KoGaMaZ
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET KoGaMaZ
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
install(FILES $<TARGET_FILE:velopack> DESTINATION ${CMAKE_INSTALL_BINDIR})

option(BUILD_TESTING "Build launcher tests" OFF)
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(launcher-tests tests/serverspage_test.cpp src/serverspage.cpp src/serverspage.h src/tilegrid.cpp src/tilegrid.h src/settings.cpp src/settings.h src/social.cpp src/social.h src/localsocialserver.cpp src/localsocialserver.h src/friendsdialog.cpp src/friendsdialog.h src/chatdialog.cpp src/chatdialog.h src/tunnel.cpp src/tunnel.h src/sharebar.cpp src/sharebar.h src/serverconfigdialog.cpp src/serverconfigdialog.h
        src/settingsdialog.cpp src/settingsdialog.h src/settingsdialog.ui
        src/gamelauncher.cpp src/gamelauncher.h src/playerprofile.cpp src/playerprofile.h src/discordpresence.cpp src/discordpresence.h src/consolewindow.cpp src/consolewindow.h src/webplayerruntime.cpp src/webplayerruntime.h src/paths.cpp src/paths.h src/theme.cpp src/theme.h
        src/launchdialog.cpp src/launchdialog.h src/launchdialog.ui src/assetinstaller.cpp src/assetinstaller.h
        src/mainwindow.cpp src/mainwindow.h src/mainwindow.ui
        src/installdialog.cpp src/installdialog.h src/installdialog.ui
        src/installprogressdialog.cpp src/installprogressdialog.h src/installprogressdialog.ui
        src/worldsdialog.cpp src/worldsdialog.h src/worldsdialog.ui
        src/admindialog.cpp src/admindialog.h src/creditsdialog.cpp src/creditsdialog.h
        src/links.cpp src/links.h src/modelpreview.cpp src/modelpreview.h
        src/brandlogo.cpp src/brandlogo.h src/playerstats.cpp src/playerstats.h
        src/profilepage.cpp src/profilepage.h
        src/appversion.h src/version.cpp src/version.h
        src/updatecheck.cpp src/updatecheck.h
        src/worldmembersdialog.cpp src/worldmembersdialog.h)
    target_include_directories(launcher-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(launcher-tests PRIVATE Qt::Widgets Qt::Network Qt::Test Qt::CorePrivate Qt::Concurrent)
    qt_add_resources(launcher-tests "test_resources" PREFIX "/" BASE resources FILES resources/logo.png resources/versions.json resources/launcher.qss resources/openkogama-font.ttf resources/discord.png resources/install.png resources/unknown.png resources/launch.png resources/lemonplus.png resources/arrow-down.svg resources/check.svg resources/plus.svg resources/plus-light.svg resources/arrow-down-light.svg)
    add_test(NAME launcher-servers COMMAND launcher-tests -o ${CMAKE_CURRENT_BINARY_DIR}/launcher-tests.txt,txt)
    set_tests_properties(launcher-servers PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Одноразовый харнесс для снимков окна (визуальная проверка UI).
    qt_add_executable(launcher-shot tests/shot_main.cpp
        src/mainwindow.cpp src/mainwindow.h src/mainwindow.ui
        src/installdialog.cpp src/installdialog.h src/installdialog.ui
        src/installprogressdialog.cpp src/installprogressdialog.h src/installprogressdialog.ui
        src/launchdialog.cpp src/launchdialog.h src/launchdialog.ui
        src/assetinstaller.cpp src/assetinstaller.h
        src/worldsdialog.cpp src/worldsdialog.h src/worldsdialog.ui
        src/serverspage.cpp src/serverspage.h src/tilegrid.cpp src/tilegrid.h
        src/gamelauncher.cpp src/gamelauncher.h src/playerprofile.cpp src/playerprofile.h
        src/discordpresence.cpp src/discordpresence.h src/links.cpp src/links.h
        src/tunnel.cpp src/tunnel.h src/sharebar.cpp src/sharebar.h
        src/serverconfigdialog.cpp src/serverconfigdialog.h
        src/admindialog.cpp src/admindialog.h src/creditsdialog.cpp src/creditsdialog.h
        src/modelpreview.cpp src/modelpreview.h src/webplayerruntime.cpp src/webplayerruntime.h
        src/settings.cpp src/settings.h src/social.cpp src/social.h
        src/localsocialserver.cpp src/localsocialserver.h
        src/friendsdialog.cpp src/friendsdialog.h src/chatdialog.cpp src/chatdialog.h
        src/settingsdialog.cpp src/settingsdialog.h src/settingsdialog.ui
        src/consolewindow.cpp src/consolewindow.h src/centereddelegate.h
        src/paths.cpp src/paths.h src/theme.cpp src/theme.h
        src/brandlogo.cpp src/brandlogo.h src/playerstats.cpp src/playerstats.h
        src/profilepage.cpp src/profilepage.h
        src/appversion.h src/version.cpp src/version.h
        src/updatecheck.cpp src/updatecheck.h
        src/worldmembersdialog.cpp src/worldmembersdialog.h)
    target_include_directories(launcher-shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(launcher-shot PRIVATE Qt::Widgets Qt::Network Qt::CorePrivate Qt::Concurrent)
    qt_add_resources(launcher-shot "shot_resources" PREFIX "/" BASE resources
        FILES resources/logo.png resources/versions.json resources/launcher.qss
              resources/openkogama-font.ttf resources/discord.png resources/install.png
              resources/unknown.png resources/launch.png resources/lemonplus.png
              resources/arrow-down.svg resources/check.svg resources/plus.svg
              resources/plus-light.svg resources/arrow-down-light.svg)

    # Встроенный соц-сервер: живой сервер + живой клиент, без моков.
    qt_add_executable(localsocial-test tests/localsocial_test.cpp
        src/localsocialserver.cpp src/localsocialserver.h
        src/social.cpp src/social.h src/settings.cpp src/settings.h
        src/tunnel.cpp src/tunnel.h src/paths.cpp src/paths.h)
    target_include_directories(localsocial-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(localsocial-test PRIVATE Qt::Widgets Qt::Network Qt::Test Qt::CorePrivate)
    add_test(NAME launcher-localsocial COMMAND localsocial-test -o ${CMAKE_CURRENT_BINARY_DIR}/localsocial-test.txt,txt)
    set_tests_properties(launcher-localsocial PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
