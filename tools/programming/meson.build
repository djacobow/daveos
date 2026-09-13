# Images are target objects: Meson builds them before invoking the programmer.
foreach backend : ['cubeprogrammer', 'openocd', 'plan']
  programming_target = backend == 'cubeprogrammer' ? 'flash' : 'flash-' + backend
  run_target(programming_target,
    command: [python, files('../flash.py'), '--backend', backend,
              '--family', platform, '--openocd', get_option('openocd'),
              '--cubeprogrammer', get_option('cubeprogrammer'),
              '--serial', get_option('probe_serial')] + firmware_images)
endforeach
