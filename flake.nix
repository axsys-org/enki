{
  description = "enki C library project with Nix, Make, Criterion, theft, and libFuzzer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    treefmt-nix.url = "github:numtide/treefmt-nix";
  };

  outputs = {
    self,
    nixpkgs,
    flake-utils,
    treefmt-nix,
  }:
    flake-utils.lib.eachDefaultSystem (
      system: let
        pkgs = import nixpkgs {inherit system;};
        lib = pkgs.lib;
        stdenv = pkgs.stdenv;
        src = lib.cleanSourceWith {
          src = ./.;
          filter = path: type: let
            base = baseNameOf path;
          in
            !(base
              == "build"
              || base == "result"
              || lib.hasPrefix "result-" base
              || base == ".direnv"
              || base == "compile_commands.json"
              || base == "compile_commands.bear.json");
        };

        treefmtEval = treefmt-nix.lib.evalModule pkgs {
          projectRootFile = "flake.nix";
          programs.clang-format.enable = true;
          programs.alejandra.enable = true;
          programs.mdformat.enable = true;
          settings.formatter.clang-format.includes = [
            "*.c"
            "*.h"
          ];
          settings.formatter.mdformat.includes = [
            "*.md"
          ];
          settings.formatter.alejandra.includes = [
            "*.nix"
          ];
        };

        compilerFor = buildType:
          if buildType == "coverage"
          then {
            compiler = pkgs.gcc;
            cc = "gcc";
          }
          else if stdenv.isDarwin
          then {
            compiler = pkgs.clang;
            cc = "clang";
          }
          else if lib.elem buildType ["debug" "release"]
          then {
            compiler = pkgs.gcc;
            cc = "gcc";
          }
          else {
            compiler = pkgs.clang;
            cc = "clang";
          };

        mkWithCompiler = buildType: let
          selected = compilerFor buildType;
        in
          import ./nix/mkenki.nix {
            inherit lib stdenv src;
            inherit (pkgs) gnumake pkg-config criterion gmp;
            inherit (selected) compiler cc;
          };

        mkenki = buildType:
          mkWithCompiler buildType {
            pname = "enki${lib.optionalString (buildType != "release") "-${buildType}"}";
            inherit buildType;
          };

        mkCheck = kind: buildType:
          mkWithCompiler buildType {
            pname = "enki-${kind}-${buildType}";
            inherit buildType;
            makeTarget =
              if kind == "unit-tests"
              then "test-unit"
              else "test-property";
            installPackage = false;
          };

        testBuildTypes = ["debug" "asan" "ubsan" "tsan"];
        testChecks =
          lib.listToAttrs
          (lib.concatMap
            (buildType: [
              {
                name = "unit-tests-${buildType}";
                value = mkCheck "unit-tests" buildType;
              }
              {
                name = "property-tests-${buildType}";
                value = mkCheck "property-tests" buildType;
              }
            ])
            testBuildTypes);

        coverageReport = stdenv.mkDerivation {
          pname = "enki-coverage-report";
          version = "0.1.0";
          inherit src;

          nativeBuildInputs = [
            pkgs.gnumake
            pkgs.pkg-config
            pkgs.lcov
            pkgs.gcovr
            (compilerFor "coverage").compiler
          ];

          buildInputs = [
            pkgs.criterion
            pkgs.gmp
          ];

          dontConfigure = true;
          buildPhase = ''
            runHook preBuild
            make coverage BUILD_TYPE=coverage CC=${(compilerFor "coverage").compiler}/bin/${(compilerFor "coverage").cc}
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out
            cp -R build/coverage/html $out/html
            cp build/coverage/coverage/enki.filtered.info $out/enki.info
            runHook postInstall
          '';
        };

        tidyCheck = stdenv.mkDerivation {
          pname = "enki-tidy";
          version = "0.1.0";
          inherit src;

          nativeBuildInputs = [
            pkgs.gnumake
            pkgs.pkg-config
            pkgs.bear
            pkgs.clang
            pkgs.clang-tools
          ];

          buildInputs = [
            pkgs.criterion
            pkgs.gmp
          ];

          dontConfigure = true;
          buildPhase = ''
            runHook preBuild
            make BUILD_TYPE=debug CC=${pkgs.clang}/bin/clang compile-commands
            make BUILD_TYPE=debug CC=${pkgs.clang}/bin/clang tidy
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out
            cp compile_commands.json $out/
            runHook postInstall
          '';
        };

        fuzzBinary = stdenv.mkDerivation {
          pname = "enki-fuzz-vector";
          version = "0.1.0";
          inherit src;

          nativeBuildInputs = [
            pkgs.gnumake
            pkgs.clang
          ];

          dontConfigure = true;
          buildPhase = ''
            runHook preBuild
            make BUILD_TYPE=asan CC=${pkgs.clang}/bin/clang fuzz-bin
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin $out/share/enki/fuzz
            cp build/asan/tests/fuzz/fuzz_vector $out/bin/enki-fuzz-vector
            cp -R tests/fuzz/corpus $out/share/enki/fuzz/corpus
            runHook postInstall
          '';
        };

        fuzzApp = pkgs.writeShellApplication {
          name = "enki-fuzz";
          text = ''
            exec ${fuzzBinary}/bin/enki-fuzz-vector ${fuzzBinary}/share/enki/fuzz/corpus "$@"
          '';
        };

        coverageApp = pkgs.writeShellApplication {
          name = "enki-coverage-report";
          text = ''
            report="${coverageReport}/html/index.html"
            if command -v xdg-open >/dev/null 2>&1 && [ -n "''${DISPLAY:-}" ]; then
              xdg-open "$report" >/dev/null 2>&1 || true
            elif command -v open >/dev/null 2>&1; then
              open "$report" >/dev/null 2>&1 || true
            fi
            printf '%s\n' "$report"
          '';
        };
      in {
        packages = {
          default = mkenki "release";
          enki = mkenki "release";
          enki-debug = mkenki "debug";
          enki-asan = mkenki "asan";
          enki-ubsan = mkenki "ubsan";
          enki-tsan = mkenki "tsan";
          enki-coverage = mkenki "coverage";
          coverage = coverageReport;
        };

        checks =
          testChecks
          // {
            # tidy = tidyCheck;
            #format = treefmtEval.config.build.check self;
            #coverage = coverageReport;
          };

        apps = {
          fuzz = flake-utils.lib.mkApp {drv = fuzzApp;};
          coverage-report = flake-utils.lib.mkApp {drv = coverageApp;};
        };

        formatter = treefmtEval.config.build.wrapper;

        devShells.default = pkgs.mkShell {
          packages =
            [
              pkgs.gcc
              pkgs.clang
              pkgs.clang-tools
              pkgs.bear
              pkgs.gnumake
              pkgs.criterion
              pkgs.gmp
              pkgs.lcov
              pkgs.gcovr
              pkgs.pkg-config
              pkgs.alejandra
              pkgs.mdformat
              treefmtEval.config.build.wrapper
            ]
            ++ lib.optionals stdenv.isLinux [
              pkgs.valgrind
              pkgs.aflplusplus
            ];

          shellHook = ''
            cat <<'BANNER'
            enki development shell

            Common targets:
              make
              make test
              make BUILD_TYPE=asan test
              make BUILD_TYPE=ubsan test
              make BUILD_TYPE=tsan test
              make coverage
              make compile-commands
              make tidy
              make format-check
              make CC=clang BUILD_TYPE=asan fuzz FUZZ_ARGS="-max_total_time=60"
            BANNER
          '';
        };
      }
    );
}
