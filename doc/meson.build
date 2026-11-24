# Manual pages (scdoc) and the Texinfo manual. Both are optional: they are
# built when their tool is found, or required with -Dman-pages=enabled /
# -Dinfo=enabled.

scdoc = find_program('scdoc', required: get_option('man-pages'))
if scdoc.found()
  foreach page : ['waybox.1', 'waybox.5']
    section = page.split('.')[-1]
    custom_target(
      page,
      input: page + '.scd',
      output: page,
      command: scdoc,
      feed: true,
      capture: true,
      install: true,
      install_dir: get_option('mandir') / 'man' + section,
      install_tag: 'man',
    )
  endforeach
endif

makeinfo = find_program('makeinfo', required: get_option('info'))
if makeinfo.found()
  custom_target(
    'waybox.info',
    input: 'waybox.texi',
    output: 'waybox.info',
    command: [makeinfo, '--no-split', '-o', '@OUTPUT@', '@INPUT@'],
    install: true,
    install_dir: get_option('infodir'),
    install_tag: 'info',
  )
endif
