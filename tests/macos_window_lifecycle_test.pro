QT += core gui network websockets widgets testlib

CONFIG += testcase c++14
TEMPLATE = app
TARGET = macos_window_lifecycle_test

INCLUDEPATH += \
    $$PWD/.. \
    $$PWD/../src \
    $$PWD/../3rdparty/qtkeychain/qtkeychain

SOURCES += \
    macos_window_lifecycle_test.cpp \
    ../src/anylink.cpp \
    ../src/common.cpp \
    ../src/configmanager.cpp \
    ../src/detaildialog.cpp \
    ../src/jsonrpcwebsocketclient.cpp \
    ../src/keychainclass.cpp \
    ../src/loading.cpp \
    ../src/profilemanager.cpp \
    ../src/textbrowser.cpp

HEADERS += \
    ../src/anylink.h \
    ../src/common.h \
    ../src/configmanager.h \
    ../src/detaildialog.h \
    ../src/jsonrpcwebsocketclient.h \
    ../src/keychainclass.h \
    ../src/loading.h \
    ../src/profilemanager.h \
    ../src/textbrowser.h

FORMS += \
    ../src/anylink.ui \
    ../src/detaildialog.ui \
    ../src/profilemanager.ui \
    ../src/textbrowser.ui

RESOURCES += \
    ../i18n.qrc \
    ../resource.qrc \
    ../icons.qrc

include(../3rdparty/qtkeychain/qtkeychain.pri)

macx {
    HEADERS += ../src/macdockiconhandler.h
    SOURCES += ../src/macdockiconhandler.mm
}
