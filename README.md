# USB Mirror

A native Windows app for mirroring an iPhone or iPad over USB. It provides a
rectangular capture window, Windows audio playback, saved settings, automatic
reconnection and optional virtual-camera output.

Version 0.4.1 uses a dark native interface, flat controls, section separators,
plain connection status and an audio level bar. It omits the header slogan and
idle footer instructions. Themed controls retain native keyboard and accessibility
providers. Windows device-change notifications trigger read-only USB-presence
updates so cached audio/video values are hidden after unplugging. The capture
engine, playback path, saved settings and separate capture window are retained.

The native USBMirror.Driver.exe replaces the link to the original driver
manager. It embeds the four verified libusb-win32 payload files, validates
Apple mobile parent IDs, checks the kernel signature before operations,
records filter backups, and checks child/class filters for unintended changes.
Only an explicit install/remove action elevates. A shared hardware lease
prevents driver changes during this app's capture session. Unknown existing
libusb0 component versions are rejected instead of overwritten. The tool does
not replace Apple's USB stack or implement the original manager's automatic
Apple-software installation, parent-driver replacement or Bonjour repair.
Install/remove and rollback still require live validation.

Both executables embed assets/USBMirror.ico. The SVG master and deterministic
tools/generate-icon.ps1 generator are included; the latter creates nine ICO
sizes and a PNG preview using Windows drawing APIs.

No bundled .NET/WPF or Python runtime is required. MSVC's runtime is linked
statically. The native capture DLL and libusb libraries are retained, along
with the original USB configuration helper and optional camera component.

## Build

The normal deliverable is one `dist/single-file/USBMirror.exe`. A native
launcher embeds the compressed runtime and prepares a content-addressed cache
under `%LOCALAPPDATA%/USBMirror/Runtime`. It verifies every cached file before
launch, repairs missing/modified files, and retains the cache so helper paths
and optional virtual-camera registrations remain usable. No updater or new
runtime framework is involved. It still uses files on disk internally.

Requirements: x64 MSVC C++ Build Tools (VS 2019 or newer) and Windows SDK. The
main upstream core is not rebuilt by this project; newer C++20 build tools are
needed to rebuild it from source.

The pinned v1.8.3 dependency is included as readable files in
[`third_party/capture-engine`](third_party/capture-engine). Its driver source
is expanded too. See [`third_party/README.md`](third_party/README.md) for
provenance and layout details. Supply the matching v1.8.3 capture runtime:

```powershell
.\build.ps1 -RuntimeSource 'C:\CaptureRuntime'
```

Use the directory containing the matching runtime DLLs and helpers. The script
copies only explicitly listed components, writes a SHA-256 provenance record,
compiles the native frontend, and runs non-capture native/runtime checks.
Headers, driver payloads and notices come directly from the vendored tree;
the build does not extract a source archive. Generated API-header copies stay
under `build/vendor`.

Developer output: `dist/USBMirror/USBMirror.exe` and `USBMirror.Driver.exe`. The build
compiles their resources with the Windows SDK resource compiler. See
`README.txt` for operation.

The USB mode selector maps Standard to upstream A/Demo (0) and Compatibility
to upstream B (1), stored as `USBMode` in the user's settings. This user's
working profile is set to Compatibility; new profiles default to Standard.
The capture engine remains unmodified. See `VALIDATION.md` for the observed
resolution difference and the limits of the live audio check.

## Validation scope

- `dist/single-file/USBMirror.exe --bundle-check <report.json>` prepares and
  verifies the runtime cache without launching the app or changing drivers.
  The report includes the cache directory and number of files written.
- `--self-test <report.json>`: aspect fitting, retry backoff and reconnect
  gates; real DLL/API loading; read-only driver environment; camera support;
  verify the wireless receiver is stopped; PCM16 input-level meter checks.
- `USBMirror.Driver.exe --self-test <report.json>`: device-ID allowlist,
  malformed-input rejection, shared-filter preservation, payload SHA-256,
  embedded icon, read-only discovery and cross-thread hardware exclusion.
  It does not run the filter installer or write driver/registry state.
- The runtime check also creates and tears down a real D3D11 preview surface
  without starting a capture session.
- `--probe <report.json>`: the above plus read-only USB/Lockdown discovery.
- `--demo`: GUI-only layout verification, with no capture core or settings
  changes.
- `--ui-test <image.bmp> [offline|streaming|waiting]`: demo interface with an
  automated control-bounds check, a screenshot of its own window, and a JSON
  report; exits automatically. Streaming/waiting data is simulated, including
  a stale-session unplug scenario. No phone capture or saved-setting changes.
- Live validation: manual USB capture, rotate, audio, window capture and
  disconnect/reconnect on the user's phone. Optional camera installation and
  capture are separate checks and are not performed automatically by tests.

The source does not change codec buffering or claim lower end-to-end latency.
Keep the working baseline available until the new frontend passes live checks.

Component sources, authorship and license notices are listed in `SOURCE.md`
and included with the application. Internal compatibility names are retained
where the capture runtime depends on them.

`tools/audio_probe.cpp` is a read-only Windows playback-session diagnostic
used during development. It reports output/mute/peak state for one process;
it is not part of the portable app and does not record audio.
