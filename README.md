# PowerScope 1.0.0

Native GTKmm3/Cairo Linux power and process monitor.

## Build (Linux Mint / Ubuntu)

```bash
sudo apt install build-essential cmake pkg-config libgtkmm-3.0-dev dpkg-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
./build/powerscope
```

## Create and install a DEB package

```bash
(cd build && cpack -G DEB)
sudo apt install ./build/powerscope_1.0.0_amd64.deb
```

Package dependencies are detected using `dpkg-shlibdeps` at packaging time. The application installs its executable, desktop launcher and scalable SVG icon.

## Settings

The app stores window size, graph range and visible graph series in `~/.config/powerscope/settings.ini` on normal exit. Settings are per-user.

## Measurements and permissions

The application reads `/proc` and `/sys`; it does not need root to launch. Intel RAPL power readings require readable `energy_uj` files; the installer deliberately does not relax sysfs permissions. Per-process wattage and energy are estimates, not direct process measurements.

## Versioning

Change `project(PowerScope VERSION ...)` in `CMakeLists.txt`; the executable window title and About dialog derive the version from the build configuration. After editing source or build configuration, run `cmake -S . -B build` then rebuild.
