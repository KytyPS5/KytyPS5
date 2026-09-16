let
  pkgs = import <nixpkgs> { };
in
import ./nix/devshell.nix { inherit pkgs; }
