{
  description = "Game Boy / Game Boy Color core for RVVM bare-metal — riscv64 freestanding, zig-cc toolchain, binjgb-vendored";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Runtime libs RVVM dlopens when launched from `make run`.
        # Same shape as zx-spectrum / chip-8 / rvvm-hal flakes — the
        # alsa-lib path needs to be on LD_LIBRARY_PATH and the
        # PipeWire alsa-bridge plugin directory must be reachable
        # via ALSA_PLUGIN_DIR for HDA → PipeWire on NixOS.
        rvvmRuntimeDeps = with pkgs; [ alsa-lib ];
        alsaPluginDir   = "${pkgs.pipewire}/lib/alsa-lib";

      in {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            zig                    # cc + linker, riscv64-freestanding-none target
            llvmPackages.bintools  # llvm-objcopy / llvm-readelf / llvm-nm
          ];

          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath rvvmRuntimeDeps;
          ALSA_PLUGIN_DIR = alsaPluginDir;

          shellHook = ''
            echo "scev-cores/game-boy: zig $(zig version)"
            echo "target: riscv64-freestanding-none, attached as RVVM mtd-physmap firmware"
            echo "audio:  libasound at $LD_LIBRARY_PATH"
            echo
            echo "build:  make"
            echo "run:    make run ROM=roms/your.gb"
          '';
        };
      });
}
