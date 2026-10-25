#!/bin/sh
# The genuine DSL 4.4.10 desktop session, run inside the /opt/dsl chroot.
# Reproduces DSL's own startup: wallpaper + torsmo + dfm + JWM (the authentic
# default), using DSL's real binaries and config files.
export HOME=/home/dsl
export DISPLAY=:0
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/X11R6/bin:/usr/games:/sbin
cd "$HOME" 2>/dev/null || cd /

# Classic DSL wallpaper; try DSL's own setters, fall back to a solid tint.
WP=/opt/.backgrounds/dsl.jpg
if [ -f "$WP" ]; then
  xsetbg -fullscreen "$WP" 2>/dev/null \
    || xli -onroot -fillscreen "$WP" 2>/dev/null \
    || Esetroot "$WP" 2>/dev/null \
    || xsetroot -solid "#36566f" 2>/dev/null
else
  xsetroot -solid "#36566f" 2>/dev/null
fi

# torsmo system monitor (DSL floats it top-right via ~/.torsmorc).
torsmo 2>/dev/null &

# dfm draws the DSL desktop icons (~/.xtdesktop).
dfm 2>/dev/null &

# JWM is DSL 4.4.10's default window manager; it reads ~/.jwmrc and friends.
exec jwm 2>/dev/null
