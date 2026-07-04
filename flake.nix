{
  description = "A terminal-based screen time and pomodoro tracker";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      pkgsFor = system: import nixpkgs { inherit system; };
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.stdenv.mkDerivation {
            pname = "ghost-watch";
            version = "1.0.0";

            src = pkgs.lib.cleanSource ./.;

            nativeBuildInputs = with pkgs; [ cmake pkg-config ];
            buildInputs = with pkgs; [ sqlite ftxui ];

            cmakeFlags = [
              "-DFETCHCONTENT_SOURCE_DIR_FTXUI=${pkgs.fetchFromGitHub {
                owner = "ArthurSonzogni";
                repo = "FTXUI";
                rev = "v7.0.0";
                sha256 = "1n15jwpypfk8pz0xk1nqjlf7ipwz111nhdzj931hx8i8swhnh3q2";
              }}"
              "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
            ];

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp ghost-watch-daemon $out/bin/
              cp ghost-watch-tui $out/bin/
              runHook postInstall
            '';
          };
        }
      );

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/ghost-watch-tui";
        };
      });
    };
}
