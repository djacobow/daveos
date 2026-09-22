# Small consumers build with only the layer they need. Each links and runs,
# and a dependency scan rejects headers from layers it must not depend on.
consumer_compiler = meson.get_compiler('cpp').cmd_array()
consumer_checks = {
  'state_machine': {
    'deps': [core_dep],
    'forbid': 'core/platform/,core/schedule/,core/logging/,core/command/,core/event/,hal/,platform/',
  },
  'driver': {
    'deps': [drivers_dep],
    'forbid': 'core/platform/,core/schedule/,core/logging/,core/command/,core/event/,platform/',
  },
  'task_app': {
    'deps': [core_dep, fake_dep],
    'forbid': 'hal/,drivers/,net/,storage/,console/,boot/,update/,otp/,watchdog/',
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
