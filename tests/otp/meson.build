otp_test = executable('test-otp', ['otp.cpp', 'flash.cpp'], dependencies: [test_support_dep, file_otp_dep, file_flash_dep])
test('otp', otp_test, suite: 'unit', timeout: 120)
