# SPDX-License-Identifier: MIT
enable_language(CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
SOAPY_SDR_MODULE_UTIL(TARGET lr2021Support SOURCES LR2021Device.cpp LIBRARIES lr2021_host)
target_compile_options(lr2021Support PRIVATE -Wall -Wextra)
if(BUILD_TESTING)
  add_executable(test_soapy test_soapy.cpp)
  target_link_libraries(test_soapy PRIVATE SoapySDR)
  target_compile_options(test_soapy PRIVATE -Wall -Wextra)
  add_test(NAME soapy_module COMMAND test_soapy)
  # Load only this build's module, not an installed copy.
  set_tests_properties(soapy_module PROPERTIES TIMEOUT 60
    ENVIRONMENT "SOAPY_SDR_PLUGIN_PATH=$<TARGET_FILE_DIR:lr2021Support>;SOAPY_SDR_ROOT=/nonexistent")
endif()
