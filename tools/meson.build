flash_script = files('flash.py')
foreach check : ['format', 'format-check', 'lint']
  run_target(check, command: [python, files('check.py'), check])
endforeach
