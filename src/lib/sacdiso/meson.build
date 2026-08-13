# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright The Music Player Daemon Project

sacdiso_dep = dependency('', required: false)

if not get_option('sacdiso')
  subdir_done()
endif

if not get_option('dsd')
  error('The SACD ISO decoder plugin requires -Ddsd=true')
endif

# DST (Direct Stream Transfer) decoder, following
# ISO/IEC 14496-3 Part 3 Subpart 10.
libdstdec = static_library(
  'dstdec',
  'libdstdec/decoder/decoder.cxx',
  'libdstdec/binding/dst_decoder.cxx',
  include_directories: inc,
)

sacdiso = static_library(
  'sacdiso',
  'Domain.cxx',
  'SacdMedia.cxx',
  'SacdDisc.cxx',
  'DstDecoder.cxx',
  include_directories: inc,
  dependencies: [
    log_dep,
    icu_dep,
  ],
)

sacdiso_dep = declare_dependency(
  link_with: [sacdiso, libdstdec],
  dependencies: [
    log_dep,
    icu_dep,
  ],
)
