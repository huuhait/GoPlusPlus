{
  description = "Go++ (gopp) development shell";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (
        pkgs:
        let
          llvm = pkgs.llvmPackages_23;
          aarch64Sysroot = pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isx86_64 {
            GOPP_AARCH64_SYSROOT = "${pkgs.pkgsCross.aarch64-multiplatform.glibc}";
          };
        in
        {
          default = pkgs.mkShell (
            {
              packages = [
                pkgs.rustc
                pkgs.cargo
                pkgs.clippy
                pkgs.rustfmt
                pkgs.cargo-fuzz
                llvm.clang
                llvm.lld
                pkgs.lldb
                pkgs.sccache
                pkgs.qemu-user
                pkgs.gdb
                pkgs.go_1_25
                pkgs.cmake
                pkgs.ninja
              ];

              GOPP_CLANG = "${llvm.clang}/bin/clang";
              LIBCLANG_PATH = "${llvm.libclang.lib}/lib";

              shellHook = ''
                export RUSTC_WRAPPER=sccache
                export SCCACHE_DIR="$HOME/.cache/sccache"
                export SCCACHE_CACHE_SIZE=100G
              '';
            }
            // aarch64Sysroot
          );
        }
      );
    };
}
