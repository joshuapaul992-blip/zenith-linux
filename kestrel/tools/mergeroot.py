#!/usr/bin/env python3
"""tools/mergeroot.py STAGE DEST -- lay Kestrel's root tree over a distribution's

DEST already holds a distribution's root file system (Alpine's minirootfs,
unpacked by the Makefile when ROOTFS_BASE is set). Every file of STAGE
(build/rootfs: Kestrel's programs, the X server, IceWM, xterm, their data)
that DEST lacks is copied in; what DEST has wins, so the distribution's own
C library, loader, shell and /etc stay as they are. Kestrel's BusyBox and
its applet links are left out: the distribution brings its own.
"""
import os
import shutil
import sys


def main():
    stage, dest = sys.argv[1], sys.argv[2]
    bb = os.path.join(stage, "bin", "busybox")
    bb_ino = os.stat(bb).st_ino if os.path.exists(bb) else None
    copied = skipped = 0
    for root, dirs, files in os.walk(stage):
        rel = os.path.relpath(root, stage)
        out = os.path.normpath(os.path.join(dest, rel))
        if not os.path.isdir(out):
            os.makedirs(out)
            shutil.copystat(root, out)
        for name in files:
            src, dst = os.path.join(root, name), os.path.join(out, name)
            st = os.lstat(src)
            if bb_ino is not None and st.st_ino == bb_ino:
                continue                                    # Kestrel's BusyBox and its applets
            if os.path.lexists(dst):
                skipped += 1
                continue
            if os.path.islink(src):
                os.symlink(os.readlink(src), dst)
            else:
                shutil.copy2(src, dst)
            copied += 1
    print(f"mergeroot: {copied} files from {stage} added, {skipped} left to the distribution")


if __name__ == "__main__":
    main()
