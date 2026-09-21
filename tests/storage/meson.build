storage_test = executable('test-storage', ['storage.cpp', 'sd_reader.cpp'],
  dependencies: [test_support_dep, storage_dep, file_block_dep])
test('storage', storage_test, suite: 'unit', timeout: 30)
