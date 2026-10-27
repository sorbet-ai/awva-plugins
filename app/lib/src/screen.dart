// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The one screen (DESIGN.md, "Layout"): the header (the mark, the engine, the preset, MIDI, the
// level, the theme, settings), the sound (its name and its hero knobs, every parameter below on
// request), the footer (audio and MIDI in one line), and the settings panel over the right edge.
// The same screen is the plugins' editor, without the engine picker and the device settings.
import 'dart:math' as math;

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'controller.dart';
import 'core.dart';
import 'param_icon.dart';
import 'theme.dart';
import 'widgets.dart';

part 'screen_footer.dart';
part 'screen_header.dart';
part 'screen_settings.dart';
part 'screen_sound.dart';

class AwvaApp extends StatelessWidget {
  const AwvaApp({super.key, required this.controller, this.brightness});
  final Controller controller;

  /// the OS's, unless given (tests)
  final Brightness? brightness;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: controller,
      builder: (context, _) {
        final os = brightness ?? MediaQuery.platformBrightnessOf(context);
        final dark = switch (controller.theme) {
          ThemeChoice.dark => true,
          ThemeChoice.light => false,
          ThemeChoice.system => os == Brightness.dark,
        };
        final colors = dark ? AwvaColors.darkTheme : AwvaColors.light;
        return AwvaTheme(
          colors: colors,
          child: WidgetsApp(
            title: 'awva',
            color: colors.sage,
            debugShowCheckedModeBanner: false,
            textStyle: AwvaType.body(colors.text1),
            pageRouteBuilder: <T>(RouteSettings s, WidgetBuilder b) => PageRouteBuilder<T>(settings: s, pageBuilder: (c, _, _) => b(c)),
            home: AwvaTheme(colors: colors, child: MainScreen(controller: controller)),
          ),
        );
      },
    );
  }
}

class MainScreen extends StatefulWidget {
  const MainScreen({super.key, required this.controller});
  final Controller controller;
  @override
  State<MainScreen> createState() => _MainScreenState();
}

class _MainScreenState extends State<MainScreen> {
  bool _settings = false, _all = false;

  Controller get k => widget.controller;

  @override
  Widget build(BuildContext context) {
    final c = AwvaTheme.of(context);
    // Ctrl+R (Cmd+R on macOS): scan the engines folder again
    return CallbackShortcuts(
      bindings: {
        const SingleActivator(LogicalKeyboardKey.keyR, control: true): k.rescan,
        const SingleActivator(LogicalKeyboardKey.keyR, meta: true): k.rescan,
        if (_settings) const SingleActivator(LogicalKeyboardKey.escape): () => setState(() => _settings = false),
      },
      child: Focus(autofocus: true, child: _screen(c)),
    );
  }

  Widget _screen(AwvaColors c) {
    return ColoredBox(
      color: c.bg0,
      child: DefaultTextStyle(
        style: AwvaType.body(c.text1),
        child: ListenableBuilder(
          listenable: k,
          builder: (context, _) => Stack(children: [
            Column(children: [
              _Header(k: k, onSettings: () => setState(() => _settings = !_settings)),
              Expanded(child: _Body(k: k, all: _all, onAll: () => setState(() => _all = !_all))),
              _Footer(k: k),
            ]),
            if (_settings) Positioned.fill(child: GestureDetector(onTap: () => setState(() => _settings = false), child: const ColoredBox(color: Color(0x00000000)))),
            if (_settings)
              Positioned(
                top: 0,
                bottom: 0,
                right: 0,
                width: 380,
                child: TweenAnimationBuilder<double>(
                  tween: Tween(begin: 1, end: 0),
                  duration: AwvaSize.open,
                  curve: Curves.easeOutCubic,
                  builder: (_, t, child) => Transform.translate(offset: Offset(t * 380, 0), child: child),
                  child: _Settings(k: k, onClose: () => setState(() => _settings = false)),
                ),
              ),
          ]),
        ),
      ),
    );
  }
}
