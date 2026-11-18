cmake_minimum_required(VERSION 3.16)
project(sshmanager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets LinguistTools)
find_package(OpenSSL REQUIRED)
find_package(qtermwidget6 REQUIRED)

add_executable(sshmanager
  src/main.cpp
  src/paths.cpp
  src/paths.h
  src/crypto.cpp
  src/crypto.h
  src/model.cpp
  src/model.h
  src/vault.cpp
  src/vault.h
  src/masterdialog.cpp
  src/masterdialog.h
  src/sshcommand.cpp
  src/sshcommand.h
  src/sessionview.cpp
  src/sessionview.h
  src/connectiondialog.cpp
  src/connectiondialog.h
  src/terminaldialog.cpp
  src/terminaldialog.h
  src/commandpalette.cpp
  src/commandpalette.h
  src/sftpclient.cpp
  src/sftpclient.h
  src/filetransferdialog.cpp
  src/filetransferdialog.h
  src/mainwindow.cpp
  src/mainwindow.h
)

target_include_directories(sshmanager PRIVATE src)
target_link_libraries(sshmanager PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets
  qtermwidget6
  OpenSSL::Crypto
)

# Compile the translator catalogs and embed them under :/i18n.
qt_add_lrelease(
  TS_FILES
    translations/sshmanager_it.ts
    translations/sshmanager_en.ts
  QM_FILES_OUTPUT_VARIABLE sshmanager_qm_files
)
foreach(qm_file IN LISTS sshmanager_qm_files)
  get_filename_component(qm_name "${qm_file}" NAME)
  set_source_files_properties("${qm_file}" PROPERTIES QT_RESOURCE_ALIAS "${qm_name}")
endforeach()
qt_add_resources(sshmanager "translations"
  PREFIX "/i18n"
  FILES ${sshmanager_qm_files}
)

install(TARGETS sshmanager RUNTIME DESTINATION bin)
