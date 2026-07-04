{ pkgs ? import <nixpkgs> {} }:

let
  ftxui_src = pkgs.fetchFromGitHub {
    owner = "ArthurSonzogni";
    repo = "FTXUI";
    rev = "v7.0.0";
    sha256 = "1n15jwpypfk8pz0xk1nqjlf7ipwz111nhdzj931hx8i8swhnh3q2";
  };
in
pkgs.stdenv.mkDerivation {
  pname = "ghost-watch";
  version = "1.0.0";

  src = pkgs.lib.cleanSource ./.;

  nativeBuildInputs = with pkgs; [
    cmake
    pkg-config
  ];

  buildInputs = with pkgs; [
    sqlite
  ];

  cmakeFlags = [
    "-DFETCHCONTENT_SOURCE_DIR_FTXUI=${ftxui_src}"
    "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
  ];

  postPatch = ''
    rm -rf build
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp ghost-watch-daemon $out/bin/
    cp ghost-watch-tui $out/bin/

    runHook postInstall
  '';
}
