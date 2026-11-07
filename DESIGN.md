# Terminal UI: design contract (v5, deep-space glass)

## 1. Intent
A competition HUD: the operator glances, reads, acts. Every pane shows real data or says
why it has none ("等待数据", "--"); nothing is drawn for data the protocol does not send.
The approved mockups are `~/图片/rm-ui-v5/1–9`; the program follows them.

## 2. Colors and materials
`cpp/theme.{h,cpp}` and `qml/Theme.qml` hold the same tokens. Backdrop: deep navy gradient,
three soft light blooms, vignette and dither (`theme::backdrop`). Panels: translucent chamfered
glass (`theme::paintGlass`, `GlassCard`, QML `GlassShape`), one cyan accent, faction red/blue.
Floating layers (urgent next-step popup, settings, editor) use the opaque overlay colour.
Health colour follows the ratio (>= 50 % green, > 25 % yellow, else red) and never goes fully
opaque. QML panels paint their own slice of the window backdrop (`image://backdrop/...`)
instead of `WA_AlwaysStackOnTop`, and use no shader effects (MultiEffect does not render
offscreen or in software); custom QML items are `QQuickPaintedItem`s (`RmTerminal.Glass`).

## 3. Typography
Numbers and Latin text: bundled Barlow Semi Condensed (OFL), tabular figures. Chinese: Noto
Sans CJK SC or the system CJK font. `theme::paintMixedText` adds a 3 px gap only where Latin
meets CJK. Sizes are set in pixels (`labelPx`, `numericPx`), so `pointSizeF()` is -1.

## 4. Geometry
The info page is a 16 × 9 unit grid with 8 px gaps (`UnitGridLayout`, `unit_rect`). A pane
gets exactly its cells, never its size hint: a crowded pane clips instead of pushing into a
neighbour. Layouts are stored as cells, not pixels. A removed or hidden pane's cells go to the
neighbours that exactly cover one of its sides (`close_hole`; the map and sideways roster growth
come last); anything left grows cell by cell (`fill_empty_cells`). Every settings edit leaves the
grid fully tiled. Window: minimum 1280×760, primary target 1440×900, tested
up to 1920×1080. Top bar 106 px.

## 5. Primitives and states
- RosterRow: NoData / Alive / Kia (grey avatar). Damage leaves a white trail (350 ms hold,
  450 ms shrink); healing flashes green (300 + 600 ms); the number flashes red for 300 ms.
  Known enemies at `0 < hp <= maxHp / 4` glow 1.0 ↔ 0.55, 650 ms per half-cycle; the operator
  can switch the glow off. HP text and bar colour still mark low health.
- BaseHealthBar: ±24° chamfered ends, italic metal numbers.
- Next step (`next_step.{h,cpp}`): fixed rule table in five groups (save life, defend, seize
  chances, resupply, score). Top 1–3 cards; a card stays at least the dwell time; a more urgent
  item may cut in; a resolved item leaves at once. Card numbers match roster badges.
- Revive popup follows the official client and sits below the next-step cards.
- Settings page (mockups 10–13): a scaled live shot of the whole window, laid out by the
  working copy. Drop on another pane's edge = insert (that pane gives up the room); drop on its
  middle = swap; an edge that cannot take it swaps too. A library pane that cannot squeeze in
  replaces the pane (which goes back to the library); a pane dropped on the library is removed;
  a pane dropped in the middle of a neighbour it cannot swap with (e.g. the video under the ally roster)
  goes on the far side of it, both keeping their size. Preview while dragging: cyan = what happens on
  release, red = nothing fits (the toast names the pane's minimum size). Seam handles between
  panes resize both sides. Gear opens details beside the pane; cross removes (any pane, one must
  stay); undo bar + Ctrl+Z. The library is one scrolling column, caption above a shot of the pane
  drawn 4 cells wide so every shot has the same width, only panes not on screen, with sample data. Card headers keep their text in
  the editor, moved left of the gear and cross.

## 6. Settings
One page: schemes along the top (a click switches at once), the live screen in the middle,
library on the left, details in a popover; alerts, export/import and renaming are under 「更多」.
Every change is saved at once; undo restores the whole seat setup step by step. The battle
analysis can show control only (`analysis_control_only`). Stored per operator and seat in
`ui_profiles.json` (version 1; new optional keys take defaults; files that cannot be read are
kept and set aside, never overwritten). Locked from the start of a match until the next
pre-match stage, also across disconnects.

## 7. Scope and verification limits
Next-step thresholds (30 % HP, 50 rounds / 5 for the hero, 45 s after the rune opens, base hit
within 5 s) are unverified initial values; rune times reuse the alert timeline and are not yet
checked against the 2027 manual; "which zone to take" ignores distance (no 2027 field map).
The global font size, panel opacity and animation switches shown in mockup 8 are not built.
Verified by ctest (offscreen), the acceptance scripts against the simulator and Xvfb
screenshots; not yet on the operators' own Wayland/GPU screens.
