class Archaic < Formula
  desc "Blazing-fast, intelligent terminal autocomplete daemon for Fish, Bash, and Zsh"
  homepage "https://github.com/Samoreilly/Archaic"
  url "https://github.com/Samoreilly/Archaic.git", branch: "main"
  license "MIT"
  head "https://github.com/Samoreilly/Archaic.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "fmt"

  def install
    system "cmake", "-S", ".", "-B", "build",
                    "-DCMAKE_BUILD_TYPE=Release",
                    *std_cmake_args
    system "cmake", "--build", "build", "--config", "Release"
    system "cmake", "--install", "build"
  end

  test do
    assert_match "archaic", shell_output("#{bin}/archaic --version")
  end
end