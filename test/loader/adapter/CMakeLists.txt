# Copyright (C) 2024 Intel Corporation
# Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.TXT
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_ur_executable(test-loader-adapter
    urAdapterSetLoggingCallback.cpp
)

target_link_libraries(test-loader-adapter
    PRIVATE
    ${PROJECT_NAME}::common
    ${PROJECT_NAME}::headers
    ${PROJECT_NAME}::loader
    gmock
    GTest::gtest_main
)

add_test(NAME loader-adapter
    COMMAND test-loader-adapter
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
