#!/usr/bin/env python3
"""tools/mergeroot.py STAGE DEST -- lay Kestrel's root tree over a distribution's

DEST already holds a distribution's root file system (Alpine's minirootfs,
unpacked by the Makefile when ROOTFS_BASE is set). Every file of STAGE
(build/rootfs: Kestrel's programs, the X server, IceWM, xterm, their data)
that DEST lacks is copied in; what DEST has wins, so the distribution's own
C library, loader, shell and /etc stay as they are. Kestrel's BusyBox goes
in only if the distribution has none (Ubuntu's root), as /bin/busybox with
links for the commands the distribution lacks (clear, ping, vi, ...); a
distribution with its own BusyBox (Alpine) keeps it and its applets.
"""
import os
import shutil
import sys


def main():
    stage, dest = sys.argv[1], sys.argv[2]
    bb = os.path.join(stage, "bin", "busybox")
    bb_ino = os.stat(bb).st_ino if os.path.exists(bb) else None
    copied = skipped = 0
    path = [os.path.join(dest, d) for d in ("bin", "sbin", "usr/bin", "usr/sbin")]
    has = lambda name: any(os.path.lexists(os.path.join(d, name)) for d in path)
    if bb_ino is not None and not has("busybox"):
        shutil.copy2(bb, os.path.join(dest, "bin", "busybox"))
        added = []
        for name in sorted(os.listdir(os.path.join(stage, "bin"))):
            if os.lstat(os.path.join(stage, "bin", name)).st_ino == bb_ino and name != "busybox" and not has(name):
                os.symlink("busybox", os.path.join(dest, "bin", name))
                added.append(name)
        print(f"mergeroot: /bin/busybox added, with {len(added)} commands the distribution lacks")
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
