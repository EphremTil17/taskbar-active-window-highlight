# Changelog

All notable changes to this mod are listed here. Versions match the mod's
`@version` and the repository's git tags.

## 0.1.1 — 2026-11-13

### Fixed

- A window minimized from the taskbar (by clicking its button) or closed while
  the taskbar had focus stayed highlighted as active. It is now cleared, and a
  minimized or closed window is never shown as active when a preview opens.

### Changed

- Removed an unload flag that had no effect: Windhawk removes the hooks
  before the mod's cleanup runs.
- Listed the symbol modules (`Taskbar.View.dll`, `ExplorerExtensions.dll`)
  above the symbol hooks, as the Windhawk catalog checks require.

### Documentation

- Stated the requirement for the Windows 11 XAML thumbnail previews, and that
  the mod has no effect with StartAllBack, ExplorerPatcher or the classic
  thumbnails.
- Added how this mod differs from Taskbar Recent Focus Highlight.
- Removed line and hook counts that would drift out of date.

## 0.1.0 — 2026-11-13

Initial release.

### Added

- Accent-colored highlight on the active window's card in the taskbar
  thumbnail preview, so the active window is visible at a glance when an app
  has several windows open.
- Settings for highlight color (Windows accent color by default, or a hex
  color), fill opacity, and outline thickness.
- "Keep the highlight while using the taskbar": the window you came from stays
  highlighted while the taskbar or a preview list has focus.

### Notes

- The active window and each card's window come from Explorer's own task list
  state; the mod does no focus tracking, polling, or timing of its own.
- Measured cost in normal use: about 0.004–0.013 ms per event, at most
  0.67 ms.
- The taskbar button keeps Windows' native active indicator.
- Tested on Windows 11 26H2, build 26300.9974.
