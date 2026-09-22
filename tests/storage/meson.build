storage_test = executable('test-storage', ['storage.cpp', 'sd_reader.cpp', package_fixture],
  cpp_args: ['-DREFERENCE_PACKAGE="' + meson.project_build_root() / 'tests/reliability/reference.ota' + '"'],
  dependencies: [test_support_dep, storage_dep, file_block_dep, update_dep, file_flash_dep])
test('storage', storage_test, suite: 'unit', timeout: 30)
