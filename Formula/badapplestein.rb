class Badapplestein < Formula
  desc "Reconstruct any video as a mosaic of source library pages"
  homepage "https://github.com/frobinson/BadApplestein"
  url "https://github.com/frobinson/BadApplestein/archive/refs/tags/v1.0.0.tar.gz"
  version "1.0.0"
  license "MIT"

  depends_on "pkg-config" => :build
  depends_on "ffmpeg"
  depends_on "mupdf" => :optional

  def install
    system "make"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    system bin/"badapplestein", "--help"
  end
end
