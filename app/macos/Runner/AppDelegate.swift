// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
import Cocoa
import FlutterMacOS

@main
class AppDelegate: FlutterAppDelegate {
  // (--hidden is MainFlutterWindow's: its window stays on screen, unseen, so this quits only when
  // a window is really closed)
  override func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    return true
  }

  override func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool {
    return true
  }
}
