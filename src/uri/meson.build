uri = static_library(
  'uri',
  'Extract.cxx',
  'QueryParser.cxx',
  'Relative.cxx',
  'Util.cxx',
  include_directories: inc,
  dependencies: [
    fmt_dep,
    util_dep,
  ]
)

uri_dep = declare_dependency(
  link_with: uri,
)
