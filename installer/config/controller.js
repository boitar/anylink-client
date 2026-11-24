function Controller()
{
    // can't use component!
    installer.setDefaultPageVisible(QInstaller.ComponentSelection,false)
    installer.finishButtonClicked.connect(this, Controller.prototype.onInstallationFinished)
//    installer.cancelMetaInfoJob()
}

Controller.prototype.IntroductionPageCallback = function()
{
//    gui.clickButton(buttons.NextButton);
    var widget = gui.currentPageWidget()
    if (installer.isUninstaller()) {
        var buttonManager = gui.findChild(widget,"PackageManagerRadioButton")
        var buttonUpdater = gui.findChild(widget,"UpdaterRadioButton")
        buttonManager.setVisible(false)
        buttonUpdater.setVisible(false)
    }
}

Controller.prototype.TargetDirectoryPageCallback = function()
{
    var widget = gui.currentPageWidget()
    widget.TargetDirectoryLineEdit.setEnabled(false)
    widget.BrowseDirectoryButton.setEnabled(false)

    if (systemInfo.kernelType === "darwin" && installer.isInstaller()) {
        // Reapply the repair-mode guard when the user navigates back from the
        // ready page and revisits the target directory page.
        installer.setValue("RemoveTargetDir", "false");
    }
}

Controller.prototype.ReadyForInstallationPageCallback = function()
{
    if (systemInfo.kernelType === "darwin" && installer.isInstaller()) {
        // The target-page guard is disabled only to allow replacement of a
        // stale uninstall.app. Restore the normal setting before IFW records
        // installation operations and writes the new maintenance tool.
        installer.setValue("RemoveTargetDir", "true");
    }
}

Controller.prototype.onInstallationFinished = function ()
{

    if (installer.isInstaller() && installer.status === QInstaller.Success) {
        if (systemInfo.kernelType === "linux") {
            installer.executeDetached("@TargetDir@/bin/anylink");
        } else if (systemInfo.kernelType === "winnt") {
            installer.executeDetached("@TargetDir@/anylink.exe");
        } else if (systemInfo.kernelType === "darwin") {
            installer.executeDetached("@TargetDir@/AnyLink.app/Contents/MacOS/AnyLink");
        }

       // if (installer.value("UILanguage") === "zh_CN") {
       //     QDesktopServices.openUrl("file:///" + installer.value("TargetDir") + "/README.html");
       // }
    }

}

//Controller.prototype.onUninstallationFinished = function ()
//{
//    try {
//        if (installer.isUninstaller()) {
//            QDesktopServices.openUrl("https://tlslink.com");
//        }
//    } catch(e) {
//        console.log(e);
//    }
//}
