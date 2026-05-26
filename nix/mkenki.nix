{
  lib,
  stdenv,
  src,
  gnumake,
  pkg-config,
  criterion,
  gmp,
  compiler,
  cc,
  extraNativeBuildInputs ? [],
}: {
  pname,
  buildType,
  makeTarget ? "all",
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
    ]
    ++ extraNativeBuildInputs;

  buildInputs = [
    criterion
    gmp

  ];

  dontConfigure = true;
  enableParallelBuilding = true;
  hardeningDisable = lib.optional (buildType == "debug") "fortify";

  buildPhase = ''
    runHook preBuild
    make ${makeTarget} BUILD_TYPE=${buildType} CC=${compiler}/bin/${cc}
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
