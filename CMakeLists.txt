cmake_minimum_required(VERSION 3.16)
project(sshmanager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network Svg LinguistTools)
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
  src/tunnelprobe.cpp
  src/tunnelprobe.h
  src/tunnelservice.cpp
  src/tunnelservice.h
  src/knownhosts.cpp
  src/knownhosts.h
  src/knownhostsdialog.cpp
  src/knownhostsdialog.h
  src/snippets.cpp
  src/snippets.h
  src/snippetdialog.cpp
  src/snippetdialog.h
  src/icons.cpp
  src/icons.h
  src/openssh.cpp
  src/openssh.h
  src/archivetransfer.cpp
  src/archivetransfer.h
  src/externaltools.cpp
  src/externaltools.h
  src/sessionend.cpp
  src/sessionend.h
  src/hostprobe.cpp
  src/hostprobe.h
  src/remoterun.cpp
  src/remoterun.h
  src/batchdialog.cpp
  src/batchdialog.h
  src/hosthealth.cpp
  src/hosthealth.h
  src/apptheme.cpp
  src/apptheme.h
)

target_include_directories(sshmanager PRIVATE src)
target_link_libraries(sshmanager PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Svg
  qtermwidget6
  OpenSSL::Crypto
)

# Some features need the qtermwidget API added by patches/qtermwidget.
# Distribution packages lack it: build without those parts instead of failing.
set(_qtw_header_text "")
get_target_property(_qtw_includes qtermwidget6 INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir IN LISTS _qtw_includes)
  foreach(_header "${_dir}/qtermwidget.h" "${_dir}/qtermwidget6/qtermwidget.h")
    if(EXISTS "${_header}")
      file(READ "${_header}" _qtw_text)
      string(APPEND _qtw_header_text "${_qtw_text}")
    endif()
  endforeach()
endforeach()

# 0002: automatic command history.
string(FIND "${_qtw_header_text}" "cursorLineText" _qtw_found)
if(NOT _qtw_found EQUAL -1)
  target_compile_definitions(sshmanager PRIVATE SSHMANAGER_QTW_CURSOR_LINE=1)
  message(STATUS "qtermwidget: cursorLineText() available, command history enabled")
else()
  message(STATUS "qtermwidget: no cursorLineText(), typed commands are not recorded "
                 "(see patches/qtermwidget)")
endif()

# 0003: exit code in the end-of-session report.
string(FIND "${_qtw_header_text}" "setExitDialogEnabled" _qtw_found)
if(NOT _qtw_found EQUAL -1)
  target_compile_definitions(sshmanager PRIVATE SSHMANAGER_QTW_EXIT_INFO=1)
  message(STATUS "qtermwidget: exit code available")
else()
  message(STATUS "qtermwidget: no exit code, session end reasons come from the output only "
                 "(see patches/qtermwidget)")
endif()

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

if(WIN32)
  # GUI subsystem: no console window. The askpass mode still works, OpenSSH
  # hands its pipes to the helper through the standard handles.
  set_target_properties(sshmanager PROPERTIES WIN32_EXECUTABLE ON)

  # Executable icon and version information (Explorer, taskbar, Properties).
  enable_language(RC)
  configure_file(resources/sshmanager.rc.in "${CMAKE_CURRENT_BINARY_DIR}/sshmanager.rc" @ONLY)
  target_sources(sshmanager PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sshmanager.rc")

  # GetExtendedTcpTable, for the tunnel listening state.
  target_link_libraries(sshmanager PRIVATE iphlpapi ws2_32)

  # Runtime files next to the executable, so the build directory can be run
  # directly (Qt itself is added with windeployqt, see README):
  #  - libqtermwidget6.dll and its keyboard layouts / color schemes
  #  - the OpenSSL libcrypto DLL
  get_filename_component(_qtw_prefix "${qtermwidget6_DIR}/../../.." ABSOLUTE)
  set(_qtw_data "${_qtw_prefix}/share/qtermwidget6")

  get_filename_component(_ossl_root "${OPENSSL_INCLUDE_DIR}/.." ABSOLUTE)
  file(GLOB _ossl_dlls "${_ossl_root}/bin/libcrypto-*.dll")

  add_custom_command(TARGET sshmanager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:qtermwidget6> ${_ossl_dlls} $<TARGET_FILE_DIR:sshmanager>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_qtw_data}/kb-layouts" "$<TARGET_FILE_DIR:sshmanager>/kb-layouts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_qtw_data}/color-schemes" "$<TARGET_FILE_DIR:sshmanager>/color-schemes"
    VERBATIM)

  install(FILES $<TARGET_FILE:qtermwidget6> ${_ossl_dlls} DESTINATION bin)
  install(DIRECTORY "${_qtw_data}/kb-layouts" "${_qtw_data}/color-schemes" DESTINATION bin)
endif()
