foreach plugin : ['rkximage', 'rockchipmpp', 'kmssrc']
  if not get_option(plugin).disabled()
    subdir(plugin)
  endif
endforeach

if rga_dep.found() and not get_option('rga').disabled()
  subdir('rockchiprga')
endif
