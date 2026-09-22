package_fixture = custom_target('reference-package', output: ['reference.ota', 'reference-b.bin', 'factory-metadata.bin', 'large.ota', 'large-b.bin'],
  command: [python, files('package_fixture.py'), '@OUTPUT0@', '@OUTPUT1@', '@OUTPUT2@', '@OUTPUT3@', '@OUTPUT4@'],
  depend_files: files('../../tools/package.py', '../../tools/factory.py'))
reliability_test = executable('test-reliability', 'reliability.cpp', 'file_flash.cpp', 'file_update.cpp', package_fixture,
  cpp_args: ['-DLARGE_PACKAGE="' + meson.current_build_dir() / 'large.ota' + '"',
             '-DLARGE_IMAGE="' + meson.current_build_dir() / 'large-b.bin' + '"',
             '-DTEST_VERSION_BUILD=' + get_option('version_build'),
             '-DTEST_BOOT_MAJOR=' + get_option('boot_version_major').to_string(),
             '-DTEST_BOOT_MINOR=' + get_option('boot_version_minor').to_string(),
             '-DREFERENCE_PACKAGE="' + meson.current_build_dir() / 'reference.ota' + '"',
             '-DREFERENCE_IMAGE="' + meson.current_build_dir() / 'reference-b.bin' + '"',
             '-DFACTORY_METADATA="' + meson.current_build_dir() / 'factory-metadata.bin' + '"'],
  dependencies: [test_support_dep, util_dep, boot_version_dep, boot_dep, update_dep, file_flash_dep])
test('reliability', reliability_test, suite: 'unit', timeout: 30)
test('factory-tool', python, args: ['-B', '-m', 'pytest', '-q', files('test_factory.py')], suite: 'unit')

test('version-tool', python, args: ['-B', '-m', 'pytest', '-q', files('test_version.py')], suite: 'unit')

test('memory-report-tool', python, args: ['-B', '-m', 'pytest', '-q', files('test_memory_report.py')], suite: 'unit')
