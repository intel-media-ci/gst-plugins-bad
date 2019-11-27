if msdk_deps.found()
  executable('test-roi', 'test-roi.c',
    include_directories : [configinc],
    dependencies: [msdk_deps, gst_dep, gstvideo_dep],
    c_args : gst_plugins_bad_args,
    install: false)
endif
