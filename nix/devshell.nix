{ pkgs }:

let
  # Build-time dependencies for the bundled SDL3 audio, video and input backends.
  buildDeps = with pkgs; [
    qt6.qtbase # Concurrent, Network, Widgets (launcher)
    vulkan-headers
    vulkan-loader
    vulkan-tools
    vulkan-validation-layers
    libglvnd
    # X11 / input
    libX11
    libXcursor
    libXext
    libXfixes
    libXi
    libXrandr
    libXScrnSaver
    libXtst
    libxkbcommon
    # Wayland
    wayland
    wayland-protocols
    libdecor
    # audio + device/dbus
    alsa-lib
    libpulseaudio
    systemd # libudev
    dbus
  ];

  # Libraries the built binaries need at runtime.
  runtimeLibs = with pkgs; [
    stdenv.cc.cc.lib # libstdc++ / libgcc_s
    libgcc.lib
    qt6.qtbase # libQt6Concurrent/Network/Widgets
    vulkan-loader
    libglvnd
    alsa-lib
    libpulseaudio
    libX11
    libXcursor
    libXext
    libXfixes
    libXi
    libXrandr
    libXScrnSaver
    libXtst
    libxkbcommon
    wayland
    libdecor
    systemd
    dbus
  ];
in
pkgs.mkShell {
  name = "kytyps5";

  nativeBuildInputs = with pkgs; [
    clang
    lld
    cmake
    ninja
    pkg-config
    git
    glslang # provides glslangValidator for the bundled shaders
  ];

  buildInputs = buildDeps;

  shellHook = ''
    export CMAKE_PREFIX_PATH="${pkgs.qt6.qtbase}:''${CMAKE_PREFIX_PATH:-}"
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/lib/qt-6/plugins:${pkgs.qt6.qtbase}/lib/qt6/plugins:''${QT_PLUGIN_PATH:-}"
    export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath runtimeLibs}:''${LD_LIBRARY_PATH:-}"

    if [ -z "''${KYTY_SHELL_QUIET:-}" ]; then
      echo "KytyPS5 dev shell"
      echo "  cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \\"
      echo "    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++"
      echo "  cmake --build _Build/linux --target launcher --parallel"
      echo "  cmake --install _Build/linux --prefix _Build/linux/install"
    fi
  '';
}
