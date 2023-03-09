// Copyright (C) 2023 Intel Corporation
// SPDX-License-Identifier: MIT

#include <uur/fixtures.h>

struct urUSMDeviceAllocTest : uur::urQueueTest {
    void SetUp() override {
        UUR_RETURN_ON_FATAL_FAILURE(uur::urQueueTest::SetUp());
        const auto deviceUSMSupport =
            uur::GetDeviceInfo<bool>(device, UR_DEVICE_INFO_USM_DEVICE_SUPPORT);
        ASSERT_TRUE(deviceUSMSupport.has_value());
        if (!deviceUSMSupport.value()) {
            GTEST_SKIP() << "Device USM is not supported.";
        }
    }
};
UUR_INSTANTIATE_DEVICE_TEST_SUITE_P(urUSMDeviceAllocTest);

TEST_P(urUSMDeviceAllocTest, Success) {
    void *ptr = nullptr;
    size_t allocation_size = sizeof(int);
    ASSERT_SUCCESS(
        urUSMDeviceAlloc(context, device, nullptr, nullptr, allocation_size, 0,
                         &ptr));
    ASSERT_NE(ptr, nullptr);

    ur_event_handle_t event = nullptr;
    uint8_t pattern = 0;
    ASSERT_SUCCESS(
        urEnqueueUSMFill(queue, ptr, sizeof(pattern), &pattern, allocation_size,
                         0, nullptr, &event));
    EXPECT_SUCCESS(urQueueFlush(queue));
    ASSERT_SUCCESS(urEventWait(1, &event));

    ASSERT_SUCCESS(urUSMFree(context, ptr));
    EXPECT_SUCCESS(urEventRelease(event));
}

TEST_P(urUSMDeviceAllocTest, InvalidNullHandleContext) {
    void *ptr = nullptr;
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_NULL_HANDLE,
                     urUSMDeviceAlloc(nullptr, device, nullptr, nullptr,
                                      sizeof(int), 0, &ptr));
}

TEST_P(urUSMDeviceAllocTest, InvalidNullHandleDevice) {
    void *ptr = nullptr;
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_DEVICE,
                     urUSMDeviceAlloc(context, nullptr, nullptr, nullptr,
                                      sizeof(int), 0, &ptr));
}

TEST_P(urUSMDeviceAllocTest, InvalidNullPtrResult) {
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_NULL_POINTER,
                     urUSMDeviceAlloc(context, device, nullptr, nullptr,
                                      sizeof(int), 0, nullptr));
}

TEST_P(urUSMDeviceAllocTest, InvalidUSMSize) {
    void *ptr = nullptr;
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_USM_SIZE,
                     urUSMDeviceAlloc(context, device, nullptr, nullptr, 13, 0,
                                      &ptr));
}

TEST_P(urUSMDeviceAllocTest, InvalidValueAlignPowerOfTwo) {
    void *ptr = nullptr;
    ASSERT_EQ_RESULT(
        UR_RESULT_ERROR_INVALID_VALUE,
        urUSMDeviceAlloc(context, device, nullptr, nullptr, sizeof(int), 1,
                         &ptr));
}
