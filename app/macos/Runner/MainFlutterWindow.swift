// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    // the command line reaches Dart's main (--no-audio, --hidden, ...)
    let project = FlutterDartProject()
    project.dartEntrypointArguments = Array(CommandLine.arguments.dropFirst())
    let flutterViewController = FlutterViewController(project: project)
    self.contentViewController = flutterViewController
    self.setContentSize(NSSize(width: 1040, height: 640))
    self.contentMinSize = NSSize(width: 720, height: 440)
    self.center()

    RegisterGeneratedPlugins(registry: flutterViewController)

    // --hidden (smoke runs on someone's machine): the window is invisible and off-screen and takes
    // no clicks; the app has no Dock icon or focus. The window is not ordered out: with no visible
    // window AppKit would quit the app at once (applicationShouldTerminateAfterLastWindowClosed).
    if CommandLine.arguments.contains("--hidden") {
      NSApp.setActivationPolicy(.prohibited)
      self.alphaValue = 0
      self.ignoresMouseEvents = true
      self.setFrameOrigin(NSPoint(x: -32000, y: -32000))
    }

    super.awakeFromNib()
  }
}
