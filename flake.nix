{
  description = "Go++ (gopp) toolchain: development environment";

  # ADR-0002: the nixpkgs revision is pinned by flake.lock; the LLVM major is
  # pinned by `llvmMajor` below. Both change only in a dedicated upgrade change.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      llvmMajor = "23";
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Same major, with LLVM_ENABLE_ASSERTIONS=ON (D0.4). This stays a Release
      # build (nixpkgs' cmakeBuildType), never Debug (about 10x slower).
      # nixpkgs has no assertions switch; `devExtraCmakeFlags` is the hook that
      # every LLVM sub-package (llvm, clang, lld, tblgen) appends to its CMake
      # flags, so LLVM, Clang and LLD agree (assertions change class layouts
      # and the ABI-breaking-checks symbol). doCheck is off so the build does
      # not also run LLVM's own test suite. LLVM_INCLUDE_BENCHMARKS=OFF: with
      # assertions, LLVM 23 defines _LIBCPP_HARDENING_MODE, which clashes with
      # the Nix cc-wrapper's own definition; the vendored google-benchmark
      # builds with -Werror and fails. Nothing here needs the benchmarks.
      withAssertions =
        llvmPackages:
        (llvmPackages.override {
          devExtraCmakeFlags = [
            "-DLLVM_ENABLE_ASSERTIONS=ON"
            "-DLLVM_INCLUDE_BENCHMARKS=OFF"
          ];
        }).overrideScope
          (_: prev: { libllvm = prev.libllvm.overrideAttrs (_: { doCheck = false; }); });

      mkDevShell =
        pkgs:
        {
          assertions ? false,
        }:
        let
          x86 = pkgs.stdenv.hostPlatform.system == "x86_64-linux";
          baseLlvm = pkgs."llvmPackages_${llvmMajor}";
          # The compiler that builds gopp is always the regular (fast, cached)
          # clang, and so are clang-format/clang-tidy. Only the LLVM/Clang/LLD
          # that gopp links against changes.
          llvm = if assertions then withAssertions baseLlvm else baseLlvm;

          # Only the clang-format / clang-tidy executables, not clang-tools'
          # lib/ dir: in the assertions shell the compiler wrapper would put
          # the regular libclang-cpp.so on gopp's RUNPATH ahead of the
          # assertions one, and gopp would not start.
          formatTools = pkgs.runCommand "gopp-format-tools" { } ''
            mkdir -p $out/bin
            for t in clang-format clang-tidy; do
              ln -s ${baseLlvm.clang-tools}/bin/$t $out/bin/$t
            done
          '';
        in
        pkgs.mkShell.override { stdenv = baseLlvm.stdenv; } {
          name = if assertions then "gopp-assertions" else "gopp";

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.sccache
            pkgs.mold
            # psutil: lit needs it for the per-test time limit (--timeout).
            (pkgs.python3.withPackages (ps: [
              ps.lit
              ps.psutil
            ]))
            pkgs.go_1_25
            formatTools
          ]
          # Local arm64 runs (P04.03, owner decision M6): user-mode QEMU runs
          # the linux/arm64 programs the tests build, against the aarch64
          # glibc below. Only on x86-64; an arm64 host runs them natively.
          ++ pkgs.lib.optionals x86 [ pkgs.qemu-user ];

          buildInputs = [
            llvm.libllvm.dev # llvm-config, LLVMConfig.cmake
            llvm.libllvm # FileCheck, not, count
            llvm.libclang.dev
            llvm.libclang
            llvm.lld.dev
            llvm.lld
            pkgs.gtest
            pkgs.zlib
            pkgs.zstd
            pkgs.libxml2
          ];

          # An assertions LLVM adds -D_LIBCPP_HARDENING_MODE=..._EXTENSIVE to
          # LLVM_DEFINITIONS; the Nix compiler wrapper would add a second,
          # different definition (..._FAST), which -Werror rejects. Let
          # LLVM's setting stand.
          hardeningDisable = pkgs.lib.optionals assertions [ "libcxxhardeningfast" ];

          # sccache is on automatically (CMake picks it up). Default: 100G
          # cache in ~/.cache/sccache, shared by every worktree. An existing
          # SCCACHE_CACHE_SIZE or SCCACHE_DIR in the caller's environment wins
          # (CI sets its own).
          shellHook = ''
            export SCCACHE_CACHE_SIZE="''${SCCACHE_CACHE_SIZE:-100G}"
          '';

          # The aarch64 glibc (crt1.o, libc, the dynamic linker): the sysroot
          # of the arm64 run tests, for `-crt-dir=$GOPP_AARCH64_SYSROOT/lib`
          # and `qemu-aarch64 -L $GOPP_AARCH64_SYSROOT`. A binary-cache
          # download (about 50 MiB with qemu-user), never built from source.
          GOPP_AARCH64_SYSROOT = pkgs.lib.optionalString x86 "${pkgs.pkgsCross.aarch64-multiplatform.glibc}";

          # find_package(LLVM/Clang/LLD) hints (see CONTRIBUTING.md).
          LLVM_DIR = "${llvm.libllvm.dev}/lib/cmake/llvm";
          Clang_DIR = "${llvm.libclang.dev}/lib/cmake/clang";
          LLD_DIR = "${llvm.lld.dev}/lib/cmake/lld";
        };
    in
    {
      devShells = forAllSystems (pkgs: {
        default = mkDevShell pkgs { };
        assertions = mkDevShell pkgs { assertions = true; };
      });
    };
}
