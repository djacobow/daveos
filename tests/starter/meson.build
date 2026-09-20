test('external-starter', python,
  args: ['-B', '-m', 'pytest', '-q', files('test_starter.py'), '--starter-board', 'host', '--starter-work', meson.current_build_dir()],
  suite: 'integration', timeout: 120)
