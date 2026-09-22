# Small consumers build with only the layer they need. Each links and runs,
# and a dependency scan rejects headers from layers it must not depend on.
consumer_compiler = meson.get_compiler('cpp').cmd_array()
consumer_checks = {
  'state_machine': {
    'deps': [core_dep],
    'forbid': 'lib/core/platform/,lib/core/schedule/,lib/core/logging/,lib/core/command/,lib/core/event/,lib/hal/,platform/',
  },
  'driver': {
    'deps': [drivers_dep],
    'forbid': 'lib/core/platform/,lib/core/schedule/,lib/core/logging/,lib/core/command/,lib/core/event/,platform/',
  },
  'task_app': {
    'deps': [core_dep, fake_dep],
    'forbid': 'lib/hal/,lib/drivers/,lib/net/,lib/storage/,lib/console/,lib/boot/,lib/update/,lib/otp/,lib/watchdog/',
  },
}
foreach name, check : consumer_checks
  source = files(name + '.cpp')
  test('consumer-' + name,
    executable('consumer-' + name, source, dependencies: check['deps']),
    suite: 'unit')
  test('layering-' + name, python,
    args: ['-B', files('check_layering.py'), meson.project_source_root(),
           source, check['forbid'], '--', consumer_compiler],
    suite: 'unit')
endforeach
