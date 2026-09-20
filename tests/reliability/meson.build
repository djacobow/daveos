package_fixture = custom_target('reference-package', output: ['reference.ota', 'reference-b.bin', 'factory-metadata.bin'],
  command: [python, files('package_fixture.py'), '@OUTPUT0@', '@OUTPUT1@', '@OUTPUT2@'],
  depend_files: files('../../tools/package.py', '../../tools/factory.py'))
reliability_test = executable('test-reliability', 'reliability.cpp', 'file_flash.cpp', package_fixture,
  cpp_args: ['-DREFERENCE_PACKAGE="' + meson.current_build_dir() / 'reference.ota' + '"',
             '-DREFERENCE_IMAGE="' + meson.current_build_dir() / 'reference-b.bin' + '"',
             '-DFACTORY_METADATA="' + meson.current_build_dir() / 'factory-metadata.bin' + '"'],
  dependencies: [test_support_dep, util_dep, boot_dep, update_dep, file_flash_dep])
test('reliability', reliability_test, suite: 'unit', timeout: 30)
test('factory-tool', python, args: files('factory_test.py'), suite: 'unit')
