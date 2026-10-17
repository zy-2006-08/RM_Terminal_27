# Terminal UI: scoped interaction contract

## 1. Intent
Preserve the existing competition HUD. Operators must distinguish actionable enemy
low health from missing intelligence, and enlarge video without defeating blinding.

## 2. Colors and materials
Reuse `qml/Theme.qml` and `cpp/theme.h`: dark layered surfaces, faction red/blue,
semantic `Theme.danger` for the low-health ring. Existing images, gradients and
neutral card backgrounds remain unchanged. No new palette or assets.

## 3. Typography
Keep PingFang SC labels and DIN Alternate numbers. Preserve the existing row scale
(11–15 px times the bounded row-height scale) and panel title hierarchy.

## 4. Geometry
Keep Panel/RobotCard/RosterRow composition and existing spacings. Rows cap at 72 px,
with 5 px gaps. The alert is an overlay: 1 px inset, 2 px border, inherited radius;
it never changes card geometry, content, or the surrounding layout.

## 5. Primitives and states
RosterRow: NoData, Alive, Kia. Only known enemies with Alive, available HP,
positive maximum, and `0 < hp <= maxHp / 4` receive the ring. Unknown allegiance,
allies, missing HP, zero/negative maximum, dead and zero HP never pulse.
VideoPane: left-click thumbnail expands the existing video page; left-click full
video clears expansion. Pointing cursor and native tooltips explain both actions.
CLI force wins over expansion; clearing expansion resumes the current automatic
mode, including blind/stale/hysteresis Video. Mode, reason and banner agree.

## 6. Motion
One border-only opacity cycle, 0.25 to 0.9 and back, 650 ms per half-cycle with
InOutSine easing. Same-length snapshot replacements preserve delegates and phase.
Motion ends as soon as eligibility ends. No card/content animation is added.

## 7. Resize and accessibility
Retain native desktop layout at 1280x720, minimum app size 1280x760, and larger
windows. HP text and bar remain readable and unchanged: motion is supplemental,
not the only health cue. No unsupported Qt style-hint property is accessed.

## 8. Scope and verification limits
No web/mobile redesign, dependencies, or broad token migration. Native Qt runtime
tests and PNG captures verify both pages and pulse phases. Existing unrelated
`BaseHealthBar.qml:7` uses unknown `SItem` in HEAD and remains out of scope.
System reduced-motion integration is not claimed; this patch removes the invalid
Qt property reference rather than adding a platform preference subsystem.
