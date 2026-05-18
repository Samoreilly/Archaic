{ stdenv, cmake, fmt, pkg-config }:

stdenv.mkDerivation rec {
  pname = "archaic";
  version = "0.9.0";

  src = ./..;

  nativeBuildInputs = [ cmake pkg-config ];
  buildInputs = [ fmt ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
  ];

  installPhase = ''
    runHook preInstall
    cmake --install . --prefix $out
    runHook postInstall
  '';

  meta = with stdenv.lib; {
    description = "Blazing-fast, intelligent terminal autocomplete daemon for Fish, Bash, and Zsh";
    homepage = "https://github.com/Samoreilly/Archaic";
    license = licenses.mit;
    platforms = platforms.linux ++ platforms.darwin;
    maintainers = [ ];
  };
}