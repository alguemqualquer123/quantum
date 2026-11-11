{
  llvmPackages,
  cmake,
  ninja,
  pkg-config,
  curl,
  dbus,
  libGL,
  libx11,
  libxcursor,
  libxi,
  libxinerama,
  vulkan-headers,
  vulkan-loader,
  python3,
  libxrandr,
  wayland,
  wayland-scanner,
  libffi,
  systemdLibs,
  lib,
  # Commit and its date from the flake, null without git info
  rev ? null,
  date ? null,
}:

let
  # date is YYYYMMDDHHMMSS
  day =
    if date == null then
      ""
    else
      "-${builtins.substring 0 4 date}-${builtins.substring 4 2 date}-${builtins.substring 6 2 date}";
in
llvmPackages.stdenv.mkDerivation {
  pname = "quantum";
  version = "0-unstable${day}";

  src = ../../.;

  nativeBuildInputs = [
    llvmPackages.clang
    cmake
    ninja
    pkg-config
  ];

  buildInputs = [
    curl
    dbus
    libGL
    libx11
    libxcursor
    libxi
    libxinerama
    vulkan-headers
    vulkan-loader
    python3
    libxrandr
    wayland
    wayland-scanner
    libffi
    systemdLibs
  ];

  # The build sees no tags, so name it after its commit; without one CMake says "unknown"
  cmakeFlags = lib.optional (rev != null) "-D QUANTUMEDITOR_VERSION=unstable${day}-g${rev}";
}
