nm = find_program('nm')
objdump = find_program('objdump')
memory_reports = []
foreach image : memory_audit_images
  memory_reports += custom_target(image.name() + '-memory',
    input: image, output: image.name() + '-memory.json',
    command: [python, files('../memory_report.py'), '--elf', '@INPUT@',
      '--nm', nm, '--objdump', objdump, '--output', '@OUTPUT@'],
    build_by_default: true)
endforeach
alias_target('memory-report', memory_reports)
