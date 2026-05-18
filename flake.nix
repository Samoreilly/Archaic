{
  description = "Blazing-fast, intelligent terminal autocomplete daemon";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systemMap = {
        x86_64-linux = "x86_64-linux";
        aarch64-linux = "aarch64-linux";
        x86_64-darwin = "x86_64-darwin";
        aarch64-darwin = "aarch64-darwin";
      };

      forAllSystems = f: nixpkgs.lib.genAttrs (builtins.attrNames systemMap) f;

      nixpkgsFor = forAllSystems (system:
        import nixpkgs {
          inherit system;
          overlays = [ self.overlays.default ];
        });
    in {
      overlays.default = final: prev: {
        archaic = final.callPackage ./default.nix { };
      };

      packages = forAllSystems (system:
        let
          pkgs = nixpkgsFor.${system};
        in {
          default = pkgs.archaic;
          archaic = pkgs.archaic;
        });

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgsFor.${system};
        in {
          default = pkgs.mkShell {
            buildInputs = with pkgs; [
              cmake
              fmt
              pkg-config
              clang
            ];
          };
        });
    };
}