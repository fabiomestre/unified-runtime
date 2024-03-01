// Copyright (C) 2024 Intel Corporation
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fixtures.h"

using urAdapterSetLoggingCallbackTest = uur::runtime::urAdapterTest;

int data = 42;
bool callbackError = false;
void loggerCallback(ur_adapter_handle_t, const char *, ur_log_level_t,
                    void *pUserData) {
    if (pUserData) {
        if (*reinterpret_cast<int *>(pUserData) != 42) {
            callbackError = true;
        }
    }
}

TEST_F(urAdapterSetLoggingCallbackTest, Success) {
    ASSERT_SUCCESS(urAdapterSetLoggingCallback(
        adapters.data(), 1, UR_LOG_LEVEL_DEBUG, loggerCallback, nullptr));
    ASSERT_FALSE(callbackError);
}

/* Tries to check if the user data is passed correctly to the UR logger.
 * Unfortunately, there is no way to make sure that the adapters will call the
 * logger. So this will just pass if there are no calls to the logger. */
TEST_F(urAdapterSetLoggingCallbackTest, SuccessUserData) {

    void *callbackUserData = reinterpret_cast<void *>(&data);
    ASSERT_SUCCESS(
        urAdapterSetLoggingCallback(adapters.data(), 1, UR_LOG_LEVEL_DEBUG,
                                    loggerCallback, callbackUserData));
    ASSERT_FALSE(callbackError);
}

TEST_F(urAdapterSetLoggingCallbackTest, NullCallback) {
    ASSERT_SUCCESS(urAdapterSetLoggingCallback(
        adapters.data(), 1, UR_LOG_LEVEL_DEBUG, nullptr, nullptr));
    ASSERT_FALSE(callbackError);
}

TEST_F(urAdapterSetLoggingCallbackTest, InvalidLevelThreshold) {
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_ENUMERATION,
                     urAdapterSetLoggingCallback(adapters.data(), 1,
                                                 UR_LOG_LEVEL_FORCE_UINT32,
                                                 nullptr, nullptr));
    ASSERT_FALSE(callbackError);
}
