// Copyright (C) 2023 Intel Corporation
// SPDX-License-Identifier: MIT

#include "fixtures.h"

using urEventRetainTest = uur::event::urEventTest;
UUR_INSTANTIATE_DEVICE_TEST_SUITE_P(urEventRetainTest);

TEST_P(urEventRetainTest, Success) {
    uint32_t prevRefCount = 0;
    ASSERT_SUCCESS(uur::GetObjectReferenceCount(event, prevRefCount));

    ASSERT_SUCCESS(urEventRetain(event));

    uint32_t refCount = 0;
    ASSERT_SUCCESS(uur::GetObjectReferenceCount(event, refCount));

    ASSERT_LT(prevRefCount, refCount);

    ASSERT_SUCCESS(urEventRelease(event));
}

TEST_P(urEventRetainTest, InvalidNullHandle) {
    ASSERT_EQ_RESULT(urEventRetain(nullptr),
                     UR_RESULT_ERROR_INVALID_NULL_HANDLE);
}
