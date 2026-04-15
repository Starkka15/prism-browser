project('gst-palm-video-decoder', 'c', version: '1.0')

gst_dep      = dependency('gstreamer-1.0',       version: '>= 1.14')
gstvideo_dep = dependency('gstreamer-video-1.0', version: '>= 1.14')

shared_library('gstpalmvideodec',
    sources: ['src/gstpalmvideodec.c'],
    dependencies: [gst_dep, gstvideo_dep],
    link_args: ['-ldl', '-lpthread'],
    install: true,
    install_dir: get_option('libdir') / 'gstreamer-1.0',
    name_prefix: '',
)
