# Maintainer: Your Name <your@email.com>
pkgname=ringlight
pkgver=1.0.0
pkgrel=1
pkgdesc="Screen ring light for KDE Plasma 6 with Howdy integration"
arch=('x86_64')
license=('MIT')
depends=('qt6-base' 'layer-shell-qt')
makedepends=('cmake')

build() {
    cd "$srcdir/.."
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
}

package() {
    cd "$srcdir/.."
    DESTDIR="$pkgdir" cmake --install build
}
