# Installed shell

Hirari remains independently runnable. When placed in Skunkworks' `System/Hirari`,
it derives InstallRoot from its executable and establishes `SKUNK_ROOT`,
`SKUNK_SYSTEM`, `SKUNK_TOOLCHAIN`, `SKUNK_DATABASE`, `SKUNK_USER`, `SKUNK_CACHE`,
`SKUNK_TEMP` and `SKUNK_LOGS`. Neither a developer checkout nor the initial working
directory selects the installed runtime.

Bundled executables come first on PATH; user and system command directories come
first on HIRARI_PATH. Duplicates and empty entries are removed. Host tools remain
available afterward unless `SKUNK_HOST_PATH=0`, which limits the tail to Windows
system utilities. Windows local user data supplies the default writable root;
`SKUNK_USER` must be absolute when supplied. `HIRARI_STATE=-` still disables
durable vocabulary for an isolated session. Explicit state overrides remain
available. Opening a world establishes its own directory and `SKUNK_WORLD`.

The Windows executable icon is generated from `icon.png` during the CMake build.
`tools/build-icon.ps1` produces 16/32/48/64/128/256 pixel PNG icon frames using
Windows' graphics facilities; the resource compiler embeds them in `hirari.exe`.
No Python, manually maintained ICO or icon path is needed at runtime.
