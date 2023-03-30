wincrypt = static_library(
  'wincrypt',
  'MD5.cxx',
  include_directories: inc,
)

cryptlib_dep = compiler.find_library('crypt32')
bcryptlib_dep = compiler.find_library('bcrypt')

wincrypt_dep = declare_dependency(
  link_with: wincrypt,
  dependencies: [cryptlib_dep, bcryptlib_dep]
)
