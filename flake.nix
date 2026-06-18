{
  description = "enki C library project with Nix, Make, Criterion, theft, and libFuzzer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    treefmt-nix.url = "github:numtide/treefmt-nix";
    reaver = {
      url = "github:sol-plunder/reaver/b8d3cb79c272e64460763c5de79ffdb245a62c1a";
      flake = false;
    };
  };

  outputs = {
    self,
    nixpkgs,
    flake-utils,
    treefmt-nix,
    reaver,
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
            inherit (pkgs) gnumake pkg-config criterion gmp lmdb openssl binutils;
            inherit (selected) compiler cc;
          };

        mkenki = buildType:
          (mkWithCompiler buildType {
            pname = "enki${lib.optionalString (buildType != "release") "-${buildType}"}";
            inherit buildType;
          }).overrideAttrs (_old: {
            installPhase = ''
              runHook preInstall
              make install BUILD_TYPE=${buildType} PREFIX=$out
              install -d $out/bin
              install -m 0755 build/${buildType}/bin/wisp $out/bin/wisp
              install -m 0755 build/${buildType}/bin/assembler $out/bin/assembler
              runHook postInstall
            '';
          });

        enkiRelease = mkenki "release";
        mkBinPackage = name: extraInstall:
          pkgs.runCommand "enki-${name}-0.1.0" {
            meta = {
              description = "${name} binary from enki";
              homepage = "https://example.invalid/enki";
              license = lib.licenses.mit;
              mainProgram = name;
              platforms = lib.platforms.unix;
            };
          } ''
            mkdir -p $out/bin
            ln -s ${enkiRelease}/bin/${name} $out/bin/${name}
            ${extraInstall}
          '';
        wispPackage = mkBinPackage "wisp" ''
          mkdir -p $out/share/enki/reaver
          cp -R ${reaver}/src $out/share/enki/reaver/src
        '';
        assemblerPackage = mkBinPackage "assembler" "";

        mkCheck = kind: buildType:
          (mkWithCompiler buildType {
            pname = "enki-${kind}-${buildType}";
            inherit buildType;
            makeTarget =
              if kind == "unit-tests"
              then "test-unit"
              else "test-property";
            installPackage = false;
          }).overrideAttrs (old:
            {
              nativeBuildInputs =
                old.nativeBuildInputs
                ++ lib.optionals (kind == "unit-tests") [
                  wispPackage
                ];
            }
            // lib.optionalAttrs (kind == "unit-tests") {
              ENKI_REAVER_SRC_DIR = "${wispPackage}/share/enki/reaver/src";
              ENKI_REAVER_PLAN_DIR = "${wispPackage}/share/enki/reaver/src/plan";
            });

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

          ENKI_REAVER_SRC_DIR = "${wispPackage}/share/enki/reaver/src";
          ENKI_REAVER_PLAN_DIR = "${wispPackage}/share/enki/reaver/src/plan";
          buildInputs = [
            pkgs.criterion
            pkgs.gmp
            pkgs.lmdb
            pkgs.openssl
            wispPackage
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
            pkgs.binutils
            pkgs.pkg-config
            pkgs.bear
            pkgs.clang
            pkgs.clang-tools
          ];

          buildInputs = [
            pkgs.criterion
            pkgs.gmp
            pkgs.lmdb
            pkgs.openssl
            pkgs.compiledb
          ];

          dontConfigure = true;
          buildPhase = ''
            runHook preBuild
            compiledb BUILD_TYPE=debug CC=${pkgs.clang}/bin/clang make
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

        # fuzzBinary = stdenv.mkDerivation {
        #   pname = "enki-fuzz-vector";
        #   version = "0.1.0";
        #   inherit src;
        #
        #   nativeBuildInputs = [
        #     pkgs.gnumake
        #     pkgs.clang
        #   ];
        #
        #   dontConfigure = true;
        #   buildPhase = ''
        #     runHook preBuild
        #     make BUILD_TYPE=asan CC=${pkgs.clang}/bin/clang fuzz-bin
        #     runHook postBuild
        #   '';
        #
        #   installPhase = ''
        #     runHook preInstall
        #     mkdir -p $out/bin $out/share/enki/fuzz
        #     cp build/asan/tests/fuzz/fuzz_vector $out/bin/enki-fuzz-vector
        #     cp -R tests/fuzz/corpus $out/share/enki/fuzz/corpus
        #     runHook postInstall
        #   '';
        # };

        # fuzzApp = pkgs.writeShellApplication {
        #   name = "enki-fuzz";
        #   text = ''
        #     exec ${fuzzBinary}/bin/enki-fuzz-vector ${fuzzBinary}/share/enki/fuzz/corpus "$@"
        #   '';
        # };

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
          default = enkiRelease;
          enki = enkiRelease;
          wisp = wispPackage;
          assembler = assemblerPackage;
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
            format = treefmtEval.config.build.check self;
            #coverage = coverageReport;
          };

        apps = {
          wisp = flake-utils.lib.mkApp {
            drv = wispPackage;
            name = "wisp";
          };
          assembler = flake-utils.lib.mkApp {
            drv = assemblerPackage;
            name = "assembler";
          };
          # fuzz = flake-utils.lib.mkApp {drv = fuzzApp;};
          coverage-report = flake-utils.lib.mkApp {drv = coverageApp;};
        };

        formatter = treefmtEval.config.build.wrapper;

        devShells.default = pkgs.mkShell {
          ENKI_REAVER_SRC_DIR = "${wispPackage}/share/enki/reaver/src";
          ENKI_REAVER_PLAN_DIR = "${wispPackage}/share/enki/reaver/src/plan";

          packages =
            [
              pkgs.gcc
              pkgs.clang
              pkgs.clang-tools
              pkgs.bear
              pkgs.gnumake
              pkgs.criterion
              pkgs.gmp
              pkgs.lmdb
              pkgs.openssl
              pkgs.lcov
              pkgs.gcovr
              pkgs.compiledb
              pkgs.pkg-config
              pkgs.alejandra
              pkgs.mdformat
              pkgs.samply
              treefmtEval.config.build.wrapper
              wispPackage
            ]
            ++ lib.optionals stdenv.isLinux [
              pkgs.valgrind
              pkgs.binutils
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
              # make CC=clang BUILD_TYPE=asan fuzz FUZZ_ARGS="-max_total_time=60"
            BANNER
          '';
        };
      }
    );
}
