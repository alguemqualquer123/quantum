{
  description = "Game engine for 2D and 3D projects with entity component system (ECS) and data-oriented design.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = (import nixpkgs { inherit system; });
          quantum = pkgs.callPackage misc/nix/package.nix {
            # Nix copies the source without .git, so git describe cannot run in the build
            rev = self.shortRev or self.dirtyShortRev or null;
            date = self.lastModifiedDate or null;
          };
        in
        {
          inherit quantum;
          default = quantum;
        }
      );

      # `nix develop` only looks at devShells, so this cannot live under packages.
      devShells = forAllSystems (
        system:
        let
          pkgs = (import nixpkgs { inherit system; });
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              cmake
              curl
              dbus
              libGL
              libffi
              libx11
              libxcursor
              libxi
              libxinerama
              libxrandr
              llvmPackages.clang
              ninja
              pkg-config
              python3
              systemdLibs
              vulkan-headers
              vulkan-loader
              wayland
              wayland-scanner
            ];
          };
        }
      );
    };
}
