{
  description = "Freestanding x86_64 Enki/Reaver unikernel toolchain";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = {nixpkgs, ...}: let
    systems = ["x86_64-linux" "x86_64-darwin" "aarch64-darwin"];
    forAllSystems = f:
      nixpkgs.lib.genAttrs systems (system: f (import nixpkgs {inherit system;}));
  in {
    devShells = forAllSystems (pkgs: {
      default = pkgs.mkShellNoCC {
        packages = with pkgs; [
          gnumake
          llvmPackages.clang-unwrapped
          llvmPackages.llvm
          lld
          python3
          qemu
        ];
        shellHook = ''
          export CC=clang
          export LD=ld.lld
          export LLVM_READELF=llvm-readelf
          export LLVM_NM=llvm-nm
        '';
      };
    });
  };
}
