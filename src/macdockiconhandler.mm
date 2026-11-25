// Copyright (c) 2011-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "macdockiconhandler.h"

#include <AppKit/AppKit.h>
#include <objc/runtime.h>

static MacDockIconHandler *s_instance = nullptr;

static BOOL dockClickHandler(id self, SEL _cmd, NSApplication *application, BOOL hasVisibleWindows) {
    Q_UNUSED(self)
    Q_UNUSED(_cmd)
    Q_UNUSED(application)
    Q_UNUSED(hasVisibleWindows)

    if (s_instance != nullptr) {
        Q_EMIT s_instance->dockIconClicked();
    }

    // Let AppKit perform its normal reopen handling after the Qt window is restored.
    return YES;
}

void setupDockClickHandler() {
    Class delClass = (Class)[[[NSApplication sharedApplication] delegate] class];
    SEL shouldHandle = sel_registerName("applicationShouldHandleReopen:hasVisibleWindows:");
    class_replaceMethod(delClass, shouldHandle, (IMP)dockClickHandler, "B@:@B");
}

MacDockIconHandler::MacDockIconHandler() : QObject()
{
    setupDockClickHandler();
}

MacDockIconHandler *MacDockIconHandler::instance()
{
    if (!s_instance)
        s_instance = new MacDockIconHandler();
    return s_instance;
}

void MacDockIconHandler::cleanup()
{
    delete s_instance;
    s_instance = nullptr;
}

/**
 * Force application activation on macOS. With Qt 5.5.1 this is required when
 * an action in the Dock menu is triggered.
 * TODO: Define a Qt version where it's no-longer necessary.
 */
void ForceActivation()
{
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
}
