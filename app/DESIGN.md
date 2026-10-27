# awva: design

The design of the awva standalone app, and of the editor the awva plugins show in a DAW (the same
screen). Everything here is in `lib/src/theme.dart`; the screens are `lib/src/screen.dart`.

## 1. Direction

The logo is three soft ribbons (sage, dusty blue, butter) laid over each other as one wave, on a
warm cream. The UI takes three things from it and nothing else:

- **The ribbon.** Values are drawn as ribbons: a thick line with round ends (knob arcs, sliders,
  the level). There are no boxes around controls, no cards, no seams and no gradients.
- **The palette.** Cream and a warm ink as the ground; the three ribbon colours, each with one job.
  Colour is rare, so a moving value, an active MIDI dot and a working limiter are what you see.
- **The softness.** Lower case labels, rounded geometric type (DM Sans), round dots, round ends.

Quiet chrome, one sound in front: the sound's name and its hero knobs fill the window; everything
else is a line at the top, a line at the bottom, and a panel on request.

## 2. Tokens

### Colour

| role | light | dark | use |
|---|---|---|---|
| bg0 | `#F6F1E7` | `#141613` | the window (light: the logo's cream) |
| bg1 | `#FBF8F2` | `#1B1D1A` | raised: menus, the settings panel |
| bg2 | `#EEE7DA` | `#232622` | hover, buttons |
| bg3 | `#E4DCCB` | `#2D312C` | tracks (knobs, sliders, level), pressed |
| hairline | `#DDD4C2` | `#343832` | the edge of a menu or panel only |
| text1 | `#2A2925` | `#F3EEE3` | names, values |
| text2 | `#66625A` | `#A7A396` | labels |
| text3 | `#8E897E` | `#6D6A61` | hints, headings in lists |
| sage | `#5F805B` | `#9EB69B` | **values and selection**: knob arcs, slider fills, the selected item, switches on |
| blue | `#5E7D95` | `#7896AD` | **signal in**: the MIDI dot, the output meter |
| butter | `#9A7B28` | `#DCC584` | **attention**: the limiter working, a port in use, a package's label |
| onSage | `#FBF8F2` | `#141613` | text on sage |
| error | `#B4483C` | `#E5866F` | refused, failed |

The dark theme uses the logo's colours as they are; the light theme darkens them just enough to
read on cream (sage 3.9:1, blue 3.9:1, butter 3.6:1 against bg0; text1 13:1, text2 5.4:1).

One job per colour: sage never warns, butter never means "selected", blue only moves with input.

The theme follows the OS's light or dark setting; the theme button in the header, or Settings >
Appearance (System, Light, Dark), overrides it, and the core keeps the choice.

### Type

DM Sans (bundled, OFL, variable), tabular figures everywhere.

| token | size / line | weight | use |
|---|---|---|---|
| micro | 11 / 14, +0.6 | 600 | section headings, lower case |
| small | 12 / 16 | 400 | hints, the footer, details |
| body | 13 / 18 | 400 | labels, lists |
| bodyStrong | 13 / 18 | 500 | buttons |
| value | 12 / 16 | 500 | a knob's value, the level |
| title | 15 / 20 | 600 | the engine and preset pickers |
| sound | 26 / 32 | 500, -0.3 | the sound's name |

### Space, shape, motion

- Spacing: 4, 8, 12, 16, 24, 32, 48. Window gutter 16; the sound's column 32 in.
- Header 56, footer 32, list rows 32.
- Radii: 6 (buttons, list rows), 10 (menus), 14 (dialogs); everything round is fully round.
- Ribbons: knob arc 5 px, sliders and the level 4 px, the meter 2 px, all with round ends.
- Elevation only for menus and the settings panel: bg1, a 1 px hairline, a 24 px soft shadow.
- Motion: 120 ms ease-out for hover and toggles; 200 ms ease-out-cubic for the settings panel.

## 3. Components

- **Knob** (76 px): a 270° track in bg3, the value as a sage arc from the minimum (from zero for a
  range that crosses zero), a short pointer at the value. The value sits inside it, the name under
  it (`_` shown as a space). Drag up or down (200 px for the whole range; Shift: 800 px), scroll
  (the wheel is the knob's alone: the page doesn't scroll under it), double click for the default. A toggle parameter is a switch instead of a knob.
- **Slider row** (in "Every parameter"): the parameter's icon (16 px) and name (180 px together), a
  4 px ribbon, the value (64 px, right). Click or drag anywhere on the ribbon; double click for the
  default.
- **Parameter icon**: every parameter leads its name with its icon, the engine's (awva
  `assets/icons`, `docs/icons.md`: a concept, or a concept with a modifier mark over a knockout of the
  background): 14 px under a knob, 16 px in a row, in the name's colour (text2), 1.5 lines on the
  24 grid with round ends and joins. `tool/gen_icons.dart` turns the pinned awva's SVGs into the
  app's `lib/src/param_icons.g.dart` and the editor's `include/awva_plugin/param_icons.h` (a test
  fails if either is stale), so both draw the same shapes. An engine without its text shows no icon.
- **Parameter tooltip**: resting 350 ms on a knob, a switch or a row's name shows the parameter's
  label (`small`, 600) and its hover text (`small`) under it: bg1, hairline, shadow, radiusSmall,
  at most 300 px wide; it goes when the pointer leaves or presses.
- **Level**: a 112 px ribbon with a round handle, -60 to +6 dB (double click: -18 dB), and under it
  two 2 px meter lines (L, R): blue, butter while the limiter works. The dB value beside it.
- **Picker**: the title in `title`, a chevron; a fill on hover; it opens a menu under it.
- **Menu**: bg1, hairline, shadow; rows 32 high with a sage dot for the current one; headings in
  `micro`; a refused item is shown, dimmed, with its reason in `error`.
- **Button**: icon only (quiet: a fill on hover) or labelled (bg2, bg3 on hover); primary: sage.
- **Switch**: a 32 × 18 ribbon, sage when on.
- **Dot**: 8 px: MIDI (blue while messages arrive), a port (sage open, butter busy, error failed).
- **Icons**: drawn, 16 px, 1.6 px lines with round ends: chevrons, settings (three ribbons with
  stops), theme (a half-filled circle), close, refresh, stop.
- **Keyboard**: every control is in the Tab order (the header left to right, then the sound): Enter
  or Space presses a button, a picker, a switch or a segment; the arrow keys turn a knob, a row's
  ribbon or the level (Shift: fine), Home and End go to the ends, Delete to the default. The
  control with the focus wears a 1.5 px sage ring 2 px outside it; Esc closes the settings panel
  or a menu. Ctrl+R (Cmd+R) rescans the engines.

## 4. Layout

```
┌ header 56 ──────────────────────────────────────────────────────────────────────────────┐
│ ≈awva≈   Bowed Strings ⌄   ‹ cello ⌄ ›                     ● MIDI   ━━━●━━ -18 dB   ◐  ☰ │
├──────────────────────────────────────────────────────────────────────────────────────────┤
│            cello                                                                         │
│            bowed strings: friction-driven waveguide strings, modal body, section         │
│                                                                                          │
│              ◜◝        ◜◝        ◜◝        ◜◝          the heroes: four to a row         │
│            force hi    pos      rosin    bright                                          │
│              ◜◝        ◜◝        ◜◝        ◜◝                                            │
│            body amt   damp     vibrato  ensemble                                         │
│                                                                                          │
│            Every parameter ›          (grouped slider rows, then "play": mono)           │
├ footer 32 ───────────────────────────────────────────────────────────────────────────────┤
│ ● Speakers · 48.0 kHz · 144 frames   CPU 4%   limiter −2.1 dB        MIDI in: controller │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

- **Header**: the logo (22 px high); the engine picker (a package shows its label in a butter
  chip); the preset picker between previous and next; then, right: MIDI, the level, the theme,
  settings. The pickers take what the rest leaves (the engine's three fifths, 240 px each at
  most) and ellipsize, title first, chip after; under 900 px the MIDI word goes, its dot stays.
- **The sound**: its name, the engine's summary, then the heroes (the preset's own, else the
  engine's; eight at most), four to a row. "Every parameter" opens the grouped list below.
- **Footer**: audio (device, rate, buffer; an error in `error`), CPU, the limiter while it works;
  right: the MIDI inputs open, or a port's problem in butter. A line in butter only where the user
  can act on it (the output chosen missing, a Bluetooth output playing shared).

Text: the figures and the controls, no explanations; a notice only where it asks for an action.
- **Settings panel** (380 px, over the right edge): Appearance; Audio output (the output picker:
  System default, then every device with its bus, Bluetooth, USB or wired; the device playing, its
  bus, backend, mode, rate; shared or exclusive and the buffer; Restart audio); the delay, key to
  sound (the figures only); MIDI inputs (each with its dot and a
  switch, a busy port's message under it); Play (mono, All notes off); Your engines (the folder,
  Rescan, each package with its engine and version, or why it was refused).
- **Engine menu**: "built in" (one line per engine), then "your engines" (the package's label,
  the engine's title at the right; a refused package dimmed with its reason).

## 5. Windows

| window | size | notes |
|---|---|---|
| standalone, default | 1040 × 640 | |
| standalone, minimum | 720 × 440 | |
| plugin editor, default | 760 × 480 | the compact screenshot |
| plugin editor, large | 1040 × 640 | |
| plugin editor, minimum | 640 × 400 | the heroes wrap to two rows of four, then scroll |

The plugin editor is this screen without the engine picker (a plugin is one engine), without the
audio and MIDI sections of Settings (the DAW owns them) and without the level (the DAW's fader).
Its preset picker, heroes, "Every parameter", theme and footer (CPU, voices) are the same.

## 6. The plugin editor

The plugins' editor (`src/editor/`) is drawn natively to this document, not with Flutter (the
evidence is in the pull request that adds the app): Direct2D and DirectWrite in a child window on
Windows, Core Graphics and Core Text in an NSView on macOS, the same layout and painting code on
both (`editor.cpp`, through a small canvas). Each binary registers its own window class (Windows) or
makes its own view class at run time (macOS), so any number of awva plugins share a host safely.

The tokens of section 2 have one source, `design/tokens.json`: `dart run tool/gen_tokens.dart`
writes the app's `lib/src/tokens.g.dart` and the editor's `include/awva_plugin/design_tokens.h`
from it, and a test fails if either is stale. The editor embeds DM Sans and the logo, so it looks
the same in every host and installs nothing.

| plugin editor | light | dark |
|---|---|---|
| the sound | `editor_<engine>_light.png` | `editor_<engine>_dark.png` |
| every parameter | `..._light_params.png` | `..._dark_params.png` |
| the preset menu | `..._light_menu.png` | `..._dark_menu.png` |
| a hero's tooltip | `..._light_tip.png` | `..._dark_tip.png` |

(`build/editor_shots/`, by `ctest -R editor_shot`; a CI artifact.)
