# hw/kestrel -- the Kestrel DDX (see kestrel.c); added by Kestrel's
# ports/xlibre/apply-patches.sh
xkestrel_server = executable(
    'Xkestrel',
    ['kestrel.c', '../../mi/miinitext.c', '../../mi/miinitext.h'],
    include_directories: inc,
    dependencies: common_dep,
    link_with: [
        libxserver_main,
        libxserver_fb,
        libxserver,
        libxserver_xkeyboard_stubs,
        libxserver_xi_stubs,
    ],
    install: true,
)
