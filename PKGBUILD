pkgname=clipstudio-clip-thumbnailer
pkgver=1.0
pkgrel=1
pkgdesc="Thumbnailer for Clip Studio Paint .clip files using Wine preview handler with Xvfb/X11 capture"
arch=('x86_64')
depends=(
  'wine'
  'xdotool'
  'imagemagick'
  'xorg-server-xvfb'
  'xorg-xwininfo'
  'libx11'
  'libxcomposite'
  'libxrender'
  'shared-mime-info'
)
makedepends=(
  'base-devel'
  'gcc'
  'mingw-w64-gcc'
  'libx11'
  'libxcomposite'
  'libxrender'
)
source=(
  'clipthumb.c'
  'grabwindow.c'
  'clip-thumbnailer.sh'
  'clip.xml'
  'clipstudio.thumbnailer'
)
sha256sums=('SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP')
build() {
  cd "$srcdir"
  x86_64-w64-mingw32-gcc clipthumb.c -municode \
    -lole32 -lshell32 -lgdi32 -luuid -ladvapi32 -lshlwapi -loleaut32 \
    -o clipthumb.exe
  gcc grabwindow.c -o grabwindow -lX11 -lXcomposite -lXrender
}
package() {
  cd "$srcdir"
  install -Dm755 clipthumb.exe "$pkgdir/usr/lib/clipthumb/clipthumb.exe"
  install -Dm755 grabwindow "$pkgdir/usr/bin/grabwindow"
  install -Dm755 clip-thumbnailer.sh "$pkgdir/usr/bin/clip-thumbnailer.sh"
  install -Dm644 clipstudio.thumbnailer "$pkgdir/usr/share/thumbnailers/clipstudio.thumbnailer"
  install -Dm644 clip.xml "$pkgdir/usr/share/mime/packages/clip.xml"
}
