# CMake generated Testfile for 
# Source directory: C:/Users/tomas/source/repos/fpga-tom/dab-lib
# Build directory: C:/Users/tomas/source/repos/fpga-tom/dab-lib/build-audio-backends
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[dab_tests]=] "C:/Users/tomas/source/repos/fpga-tom/dab-lib/build-audio-backends/Debug/dab_tests.exe")
  set_tests_properties([=[dab_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/tomas/source/repos/fpga-tom/dab-lib/CMakeLists.txt;627;add_test;C:/Users/tomas/source/repos/fpga-tom/dab-lib/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[dab_tests]=] "C:/Users/tomas/source/repos/fpga-tom/dab-lib/build-audio-backends/Release/dab_tests.exe")
  set_tests_properties([=[dab_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/tomas/source/repos/fpga-tom/dab-lib/CMakeLists.txt;627;add_test;C:/Users/tomas/source/repos/fpga-tom/dab-lib/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[dab_tests]=] "C:/Users/tomas/source/repos/fpga-tom/dab-lib/build-audio-backends/MinSizeRel/dab_tests.exe")
  set_tests_properties([=[dab_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/tomas/source/repos/fpga-tom/dab-lib/CMakeLists.txt;627;add_test;C:/Users/tomas/source/repos/fpga-tom/dab-lib/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[dab_tests]=] "C:/Users/tomas/source/repos/fpga-tom/dab-lib/build-audio-backends/RelWithDebInfo/dab_tests.exe")
  set_tests_properties([=[dab_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/tomas/source/repos/fpga-tom/dab-lib/CMakeLists.txt;627;add_test;C:/Users/tomas/source/repos/fpga-tom/dab-lib/CMakeLists.txt;0;")
else()
  add_test([=[dab_tests]=] NOT_AVAILABLE)
endif()
