# Notes for AI agents

Compositor is a macOS image editor for compositing and photo work, written in Swift (SwiftUI and AppKit, with some C for pixel work).

## Designing or editing a Compositor project

If you've been asked to make or change an image in a `.comp` project, you don't need the app's source code. Read [docs/writing-comp-files.md](docs/writing-comp-files.md): it covers the file format, the rules that make a project load, and how to write it safely while it's open, so the person can watch the canvas update as you work.

## Working on the app itself

- Build: open `Compositor.xcodeproj` and run the **Compositor** scheme, or `xcodebuild -project Compositor.xcodeproj -scheme Compositor -destination 'platform=macOS' build`.
- Tests: the `CompositorTests` target (`xcodebuild ... test -only-testing:CompositorTests`). CI runs these on every push.
- Match the surrounding code: its naming, its comment style and density.
- American spelling in code, comments and UI ("color", not "colour").
- The project file format is described in [docs/project-format.md](docs/project-format.md). A change to what's saved means a format version bump there and in `ProjectManifest.current`.

## Working on the Windows version

- The Windows app lives in `windows/`: a C++17 core library (no Qt), a Qt 6 Widgets UI, a CLI and tests. It compiles the C kernels in `Compositor/Rendering/` directly, so a change to a kernel affects both apps.
- Build and test (any platform for the core; Windows for the full app): `cmake -S windows -B build -G Ninja && cmake --build build && ctest --test-dir build`. Pass `-DCOMPOSITOR_BUILD_APP=OFF` to skip the Qt UI. The UI smoke test needs `QT_QPA_PLATFORM=offscreen` on machines without a display.
- `.github/workflows/windows.yml` builds with MSVC, runs the tests and packages a portable zip and an installer.
- Its README, code comments and UI text are in Chinese. A format version bump also means updating `kFormatVersion` in `windows/core/include/compositor/document.h`.
