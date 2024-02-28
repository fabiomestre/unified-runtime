// Copyright (C) 2024 Intel Corporation
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ur_api.h"
#include <gtest/gtest.h>

#ifndef ASSERT_SUCCESS
#define ASSERT_SUCCESS(ACTUAL) ASSERT_EQ(UR_RESULT_SUCCESS, ACTUAL)
#endif

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

struct LoaderAdapterTest : ::testing::Test {
    void SetUp() override {
        ASSERT_SUCCESS(urLoaderInit(0, nullptr));
        ASSERT_SUCCESS(urAdapterGet(0, nullptr, &adapterCount));
        adapters.resize(adapterCount);
        ASSERT_SUCCESS(urAdapterGet(adapterCount, adapters.data(), nullptr));
    }

    void TearDown() override { ASSERT_SUCCESS(urLoaderTearDown()); }

    std::vector<ur_adapter_handle_t> adapters;
    uint32_t adapterCount = 0;
};

using LoaderAdapterSetLoggingCallbackTest = LoaderAdapterTest;

TEST_F(LoaderAdapterSetLoggingCallbackTest, Success) {
    ASSERT_SUCCESS(urAdapterSetLoggingCallback(adapters.data(), adapterCount,
                                               loggerCallback, &data));
    urAdapterGet(adapterCount, adapters.data(), nullptr);
    ASSERT_FALSE(callbackError);
}
