
Debian
====================
This directory contains files used to package btrd/btr-qt
for Debian-based Linux systems. If you compile btrd/btr-qt yourself, there are some useful files here.

## btr: URI support ##


btr-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install btr-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your btr-qt binary to `/usr/bin`
and the `../../share/pixmaps/btr128.png` to `/usr/share/pixmaps`

btr-qt.protocol (KDE)

