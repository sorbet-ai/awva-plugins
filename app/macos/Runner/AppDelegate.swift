// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
import Cocoa
import FlutterMacOS

@main
class AppDelegate: FlutterAppDelegate {
  // (FlutterAppDelegate declares applicationDidFinishLaunching but doesn't implement it: an
  // override, without a super call, which would be an unrecognized selector)
  override func applicationDidFinishLaunching(_ notification: Notification) {
    // --hidden: no window, no Dock icon (smoke runs on someone's machine)
    if CommandLine.arguments.contains("--hidden") {
      NSApp.setActivationPolicy(.prohibited)
      mainFlutterWindow?.orderOut(nil)
    }
  }

  override func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    return true
  }

  override func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool {
    return true
  }
}
