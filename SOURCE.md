# Source and baseline

USB Mirror is a new native Win32 frontend licensed under GPL-3.0-only. It uses
the unmodified native capture engine, USB configuration helper, and optional
virtual-camera component from the user's working iPhoneMirror v1.8.3 install.

- Upstream: https://github.com/RayrenSX/iPhoneMirror
- Source tag: https://github.com/RayrenSX/iPhoneMirror/tree/v1.8.3
- Tagged source archive: https://codeload.github.com/RayrenSX/iPhoneMirror/zip/refs/tags/v1.8.3
- Original authors and component notices: licenses/upstream-notices.md
- Exact copied runtime hashes: runtime-provenance.json

The local project contains `src/main.cpp`, `src/logic.h`, `src/ui_theme.h`, the application
manifest, `build.ps1`, and the complete upstream source snapshot under
`third_party/capture-engine/`. The build copies the canonical API headers
from that tree into the ignored `build/vendor/` directory. The small frontend is compiled
with MSVC C++17 and the Windows SDK. The original capture engine is copied
without recompilation to preserve the exact baseline that works on this phone.

The packaged binary does not contain the upstream managed UI, Python control
bridge, wireless runtime, or recording/streaming encoder. Do not distribute a
modified binary package without its corresponding frontend and upstream
component sources and license notices.

USBMirror.Driver.exe is a new GPL native driver-manager frontend/backend.
Its Apple product-ID allowlist, payload SHA-256 constants and selected-device
filter/backup approach are adapted from upstream src/DriverInstaller. It
embeds the unmodified install-filter.exe, libusb0.sys, amd64 libusb0.dll and
x86 libusb0_x86.dll from the v1.8.3 snapshot. Their full corresponding source
is expanded under `third_party/capture-engine/third_party/libusb-win32/source/`,
including its GPL/LGPL license files. Driver GPL/LGPL notices are included
under licenses/driver-* in the binary package.

The icon master, PNG and ICO are in assets/. Its reproducible Windows drawing
generator is tools/generate-icon.ps1. The icon is compiled into each executable
through src/app.rc and is also distributed separately as USBMirror.ico.

The single-file edition adds src/launcher.cpp, src/bundle_pack.cpp and
src/bundle_format.h. The launcher embeds the compressed, complete native
runtime including the license/source notices. It uses the Windows Compression
API and per-file SHA-256 checks, then runs the same application from a private
cache in LocalAppData. The cache is retained between runs. The standalone
executable does not statically merge the upstream DLLs or rewrite their code.
Corresponding sources are available as ordinary files in this repository.

The repository replaces the previously bundled capture-engine-1.8.3.zip
with the readable source tree. `third_party/README.md` records the original
archive hashes and the expanded driver-source location. Historical local
release ZIPs are ignored by Git. Product-facing text uses USB Mirror
branding. Existing copyright, license and source notices remain here
and in the bundled notices. Internal helper names, API namespaces and
interoperability locks are retained where required for compatibility.
The camera administrator helper is packaged as USBMirror.CameraSetup.exe;
its contents are unchanged and its original filename is recorded in the
runtime provenance manifest.
