# Dependency for Google Test
gtest_dep = dependency('gtest', main : true, required : false)
if not gtest_dep.found()
  # Ensure you have 'gtest.wrap' in your subprojects/ directory 
  # or run `meson wrap install gtest`
  gtest_proj = subproject('gtest')
  gtest_dep = gtest_proj.get_variable('gtest_main_dep')
endif

# The Test Executable
test_avatree_exe = executable('test_avatree',
  sources : ['test_avatree.cpp', '../src/avatree.c', '../src/avapager.c'], # Link the C source directly
  include_directories : include_directories('../include'),
  dependencies : [gtest_dep],
  cpp_args : '-DAVA_TESTING'
)

# Register with 'meson test'
test('avatree_tests', test_avatree_exe)