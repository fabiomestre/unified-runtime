// Copyright (C) 2023 Intel Corporation
// SPDX-License-Identifier: MIT

#include <cstring>
#include <uur/fixtures.h>

struct urUSMHostAllocTest : uur::urQueueTest {
    void SetUp() override {
        UUR_RETURN_ON_FATAL_FAILURE(uur::urQueueTest::SetUp());
        const auto hostUSMSupport =
            uur::GetDeviceInfo<bool>(device, UR_DEVICE_INFO_USM_HOST_SUPPORT);
        ASSERT_TRUE(hostUSMSupport.has_value());
        if (!hostUSMSupport.value()) {
            GTEST_SKIP() << "Device USM is not supported.";
        }
    }
};
UUR_INSTANTIATE_DEVICE_TEST_SUITE_P(urUSMHostAllocTest);

TEST_P(urUSMHostAllocTest, Success) {
    const auto hostUSMSupport = uur::GetDeviceInfo<bool>(
        this->device, UR_DEVICE_INFO_HOST_UNIFIED_MEMORY);
    ASSERT_TRUE(hostUSMSupport.has_value());
    if (!hostUSMSupport.value()) {
        GTEST_SKIP() << "Host USM is not supported.";
    }

    size_t allocation_size = sizeof(int);
    int *ptr = nullptr;
    ASSERT_SUCCESS(urUSMHostAlloc(context, nullptr, nullptr, allocation_size, 0,
                                  reinterpret_cast<void **>(&ptr)));
    ASSERT_NE(ptr, nullptr);

    // Set 0
    ur_event_handle_t event = nullptr;

    uint8_t pattern = 0;
    ASSERT_SUCCESS(
        urEnqueueUSMFill(queue, ptr, sizeof(pattern), &pattern, allocation_size,
                         0, nullptr, &event));
    EXPECT_SUCCESS(urQueueFlush(queue));
    ASSERT_SUCCESS(urEventWait(1, &event));
    EXPECT_SUCCESS(urEventRelease(event));
    ASSERT_EQ(*ptr, 0);

    // Set 1, in all bytes of int
    pattern = 1;
    ASSERT_SUCCESS(
        urEnqueueUSMFill(queue, ptr, sizeof(pattern), &pattern, allocation_size,
                         0, nullptr, &event));
    EXPECT_SUCCESS(urQueueFlush(queue));
    ASSERT_SUCCESS(urEventWait(1, &event));
    EXPECT_SUCCESS(urEventRelease(event));
    // replicate it on host
    int set_data = 0;
    std::memset(&set_data, 1, allocation_size);
    ASSERT_EQ(*ptr, set_data);

    ASSERT_SUCCESS(urUSMFree(context, ptr));
}

TEST_P(urUSMHostAllocTest, InvalidNullHandleContext) {
    void *ptr = nullptr;
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_NULL_HANDLE,
                     urUSMHostAlloc(nullptr, nullptr, nullptr, sizeof(int), 0,
                                    &ptr));
}

TEST_P(urUSMHostAllocTest, InvalidNullPtrMem) {
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_NULL_POINTER,
                     urUSMHostAlloc(context, nullptr, nullptr, sizeof(int), 0,
                                    nullptr));
}

TEST_P(urUSMHostAllocTest, InvalidUSMSize) {
    void *ptr = nullptr;
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_USM_SIZE,
                     urUSMHostAlloc(context, nullptr, nullptr, 13, 0, &ptr));
}

TEST_P(urUSMHostAllocTest, InvalidValueAlignPowerOfTwo) {
    void *ptr = nullptr;
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_VALUE,
                     urUSMHostAlloc(context, nullptr, nullptr, sizeof(int), 1,
                                    &ptr));
}
