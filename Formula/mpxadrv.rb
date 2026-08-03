# Homebrew formula for mpxadrv (source install).
#
# From a clone:
#   brew install --HEAD --formula ./Formula/mpxadrv.rb
#
# Without cloning:
#   brew install --HEAD --formula \
#     https://raw.githubusercontent.com/YosAwed/mpxadrv/main/Formula/mpxadrv.rb
#
# After a GitHub release tag vX.Y.Z, add a stable block:
#   url "https://github.com/YosAwed/mpxadrv/archive/refs/tags/vX.Y.Z.tar.gz"
#   sha256 "<sha256 of that tarball>"
#   version "X.Y.Z"
class Mpxadrv < Formula
  desc "Native macOS CLI player for MADRV/MXDRV MDX and MDR music"
  homepage "https://github.com/YosAwed/mpxadrv"
  head "https://github.com/YosAwed/mpxadrv.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "fluid-synth"
  depends_on "mdxmini"
  depends_on :macos

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           *std_cmake_args
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"
  end

  def caveats
    <<~EOS
      Optional SC-55-style SoundFonts are not bundled. See the project README
      for downloading Roland_SC-55.sf2 into SoundFonts/ or pass --soundfont.

      Interactive menu (after install):
        mpxadrv-player /path/to/music
    EOS
  end

  test do
    assert_match "Awed", shell_output("#{bin}/mpxadrv --version")
    assert_predicate bin/"mpxadrv-player", :executable?
  end
end
