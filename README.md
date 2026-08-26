# UFS2Xplorer

A desktop app for reading and managing a PS3 hard drive on your PC.

Take the drive out of your PS3 (or connect it with a USB adapter), open it on your
computer, and browse it like a file manager. You can copy files in and out, install
PKG games, updates and DLC straight onto the disk, install their licenses, and back
up or rebuild the game database. It reads and writes the PS3's encrypted GameOS
partition (UFS2), so nothing has to be done on the console itself apart from a final
database rebuild.

There are already good command line readers for PS3 drives. UFS2Xplorer is aimed at
people who want a proper graphical app that can also write, not just read.

## What it can do

- Browse the whole drive in a tree view (copy, cut, paste, rename, extract, import)
- Install PKG files (games, updates, DLC), on their own or in a batch
- Install licenses from RAP files, for every user on the console at once
- Back up and restore the XMB game database, or flag it for a rebuild
- Check and repair the filesystem's free space accounting
- Edit a game's PARAM.SFO fields
- Safely eject the drive when you are done

Windows for now. Linux and macOS are planned.

## Warning

This writes directly to your PS3 hard drive. A wrong key, a bad cable, or pulling the
drive at the wrong moment can corrupt it. Back up anything you care about first, always
use the Eject button before unplugging, and do not interrupt an install. You use this
at your own risk.

## Getting started

1. Connect your PS3 drive to your PC and run UFS2Xplorer as administrator.
2. Click **Add Drive** and follow the wizard: pick the disk and enter your console's
   EID key. Add your IDPS and account id too if you want to install licenses.
3. Select the drive and click **Open**.

The full walkthrough, including where to get your keys, is in [SETUP.md](SETUP.md).

Building from source is covered in [docs/BUILDING.md](docs/BUILDING.md).

## Third-party components

- **Qt 6** (Qt Company) under the LGPL v3.
- **OpenSSL** under the Apache License 2.0.
- **Catch2** under the Boost Software License 1.0 (used only by the tests).
- **Silk icon set 1.3** by Mark James, under CC BY 2.5.

The PS3 curve parameters, keys, and key-derivation tables used for licensing are
long-published public values. The cryptography here is built on OpenSSL; no
third-party code is bundled for it.

## Credits

Not affiliated with Sony Interactive Entertainment. For use with your own console and
your own backups only.
