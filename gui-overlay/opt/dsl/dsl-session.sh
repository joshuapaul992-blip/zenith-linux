#!/bin/sh
# The genuine DSL 4.4.10 desktop session, run inside the /opt/dsl chroot.
# Ubuntu's X server runs outside the chroot, so we find a reachable DISPLAY
# (TCP localhost, or the unix socket if it was bind-mounted in) and launch
# DSL's real JWM + torsmo + dfm against it.
export HOME=/home/dsl
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/X11R6/bin:/usr/games:/sbin
cd "$HOME" 2>/dev/null || cd /

XSETROOT=/usr/X11R6/bin/xsetroot
echo "=== DSL session starting ==="
echo "X unix sockets visible: $(ls /tmp/.X11-unix/ 2>/dev/null || echo none)"

# Probe candidate displays; the first that accepts a connection wins.
DISPLAY_OK=""
for disp in ":0" "unix:0" "127.0.0.1:0"; do
  if DISPLAY="$disp" "$XSETROOT" -solid "#36566f" 2>/tmp/xconn.err; then
    DISPLAY_OK="$disp"
    echo "connected to X via DISPLAY=$disp"
    break
  fi
  echo "no connection via DISPLAY=$disp: $(cat /tmp/xconn.err 2>/dev/null)"
done

if [ -z "$DISPLAY_OK" ]; then
  echo "=== DSL session: could NOT reach the X server on any display ==="
  exit 1
fi
export DISPLAY="$DISPLAY_OK"

# Classic DSL wallpaper on top of the blue root.
WP=/opt/.backgrounds/dsl.jpg
if [ -f "$WP" ]; then
  xsetbg -fullscreen "$WP" 2>/dev/null \
    || xli -onroot -fillscreen "$WP" 2>/dev/null \
    || Esetroot "$WP" 2>/dev/null || true
fi

torsmo 2>/dev/null &   # system monitor (top-right, via ~/.torsmorc)
dfm 2>/dev/null &      # desktop icons (~/.xtdesktop)

echo "=== launching JWM on $DISPLAY ==="
exec jwm
