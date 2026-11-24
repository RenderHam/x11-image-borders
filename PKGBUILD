# Maintainer: RenderHam

pkgname=xib
pkgver=1.0
pkgrel=1
pkgdesc='X11 daemon drawing 9-slice PNG borders around managed windows'
arch=('x86_64' 'aarch64')
url='https://github.com/RenderHam/x11-image-borders'
depends=('cairo' 'libxcb')
makedepends=('clang' 'make' 'pkgconf')
source=("https://github.com/RenderHam/x11-image-borders/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('075b5b57c63a988b3d50e84ea1a7dfaad746996ba97ddca3cb50e61bd9d132ab')

build() {
    make -C "$srcdir"/x11-image-borders-*
}

check() {
    (cd "$srcdir"/x11-image-borders-* && ./xib --render-test)
}

package() {
    make -C "$srcdir"/x11-image-borders-* DESTDIR="$pkgdir" PREFIX=/usr install
}
