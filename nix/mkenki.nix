{
  lib,
  stdenv,
  src,
  gnumake,
  pkg-config,
  criterion,
  gmp,
  lmdb,
  openssl,
  binutils,
  compiler,
  cc,
  extraNativeBuildInputs ? [],
}: {
  pname,
  buildType,
  makeTarget ? "all",
  makeArgs ? "",
  installPackage ? true,
}:
stdenv.mkDerivation {
  inherit pname src;
  version = "0.1.0";

  nativeBuildInputs =
    [
      gnumake
      pkg-config
      compiler
      binutils
    ]
    ++ extraNativeBuildInputs;

  buildInputs = [
    criterion
    gmp
    lmdb
    openssl
  ];

  dontConfigure = true;
  enableParallelBuilding = true;
  hardeningDisable = ["fortify" "fortify3"];

  buildPhase = ''
    runHook preBuild
    make ${makeTarget} BUILD_TYPE=${buildType} CC=${compiler}/bin/${cc} ${makeArgs}
    runHook postBuild
  '';

  installPhase =
    if installPackage
    then ''
      runHook preInstall
      make install BUILD_TYPE=${buildType} PREFIX=$out
      runHook postInstall
    ''
    else ''
      runHook preInstall
      mkdir -p $out
      printf '%s\n' '${pname} passed' > $out/result.txt
      runHook postInstall
    '';

  meta = {
    description = "Small C11 dynamic array library with injected allocation";
    homepage = "https://example.invalid/enki";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
  };
}
