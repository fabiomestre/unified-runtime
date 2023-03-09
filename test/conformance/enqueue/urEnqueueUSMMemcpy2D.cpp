// Copyright (C) 2023 Intel Corporation
// SPDX-License-Identifier: MIT

#include "helpers.h"
#include <uur/fixtures.h>

struct urEnqueueUSMMemcpy2DTestWithParam
    : uur::urQueueTestWithParam<uur::TestParameters2D> {
    void SetUp() override {
        UUR_RETURN_ON_FATAL_FAILURE(
            uur::urQueueTestWithParam<uur::TestParameters2D>::SetUp());
        const auto device_usm =
            uur::GetDeviceInfo<bool>(device, UR_DEVICE_INFO_USM_DEVICE_SUPPORT);
        ASSERT_TRUE(device_usm.has_value());
        if (!device_usm.value()) {
            GTEST_SKIP() << "Device USM not supported.";
        }

        const auto [inPitch, inWidth, inHeight] = getParam();
        std::tie(pitch, width, height) =
            std::make_tuple(inPitch, inWidth, inHeight);

        const size_t num_elements = pitch * height;
        ASSERT_SUCCESS(
            urUSMDeviceAlloc(context, device, nullptr, nullptr, num_elements, 0,
                             &pSrc));
        ASSERT_SUCCESS(
            urUSMDeviceAlloc(context, device, nullptr, nullptr, num_elements, 0,
                             &pDst));
        ur_event_handle_t memset_event = nullptr;

        ASSERT_SUCCESS(
            urEnqueueUSMFill2D(queue, pSrc, pitch, sizeof(memset_value),
                               &memset_value, width, height, 0, nullptr,
                               &memset_event));

        ASSERT_SUCCESS(urQueueFlush(queue));
        ASSERT_SUCCESS(urEventWait(1, &memset_event));
        ASSERT_SUCCESS(urEventRelease(memset_event));
    }

    void TearDown() override {
        uur::urQueueTestWithParam<uur::TestParameters2D>::TearDown();
        if (pSrc) {
            ASSERT_SUCCESS(urUSMFree(context, pSrc));
        }
        if (pDst) {
            ASSERT_SUCCESS(urUSMFree(context, pDst));
        }
    }

    void verifyMemcpySucceeded() {
        std::vector<uint8_t> host_mem(pitch * height);
        ASSERT_SUCCESS(urEnqueueUSMMemcpy2D(queue, true, host_mem.data(), pitch,
                                            pDst, pitch, width, height, 0,
                                            nullptr, nullptr));
        for (size_t w = 0; w < width; ++w) {
            for (size_t h = 0; h < height; ++h) {
                const auto *host_ptr = host_mem.data();
                const size_t index = (pitch * h) + w;
                ASSERT_TRUE(*(host_ptr + index) == memset_value);
            }
        }
    }

    void *pSrc = nullptr;
    void *pDst = nullptr;
    static constexpr uint8_t memset_value = 42;
    size_t pitch = 0;
    size_t width = 0;
    size_t height = 0;
};

static std::vector<uur::TestParameters2D> test_cases{
    /* Everything set to 1 */
    {1, 1, 1},
    /* Height == 1 && Pitch > width */
    {1024, 256, 1},
    /* Height == 1 && Pitch == width */
    {1024, 1024, 1},
    /* Height > 1 && Pitch > width */
    {1024, 256, 256},
    /* Height > 1 && Pitch == width + 1 */
    {234, 233, 23},
    /* Height == 1 && Pitch == width + 1 */
    {234, 233, 1}};

UUR_TEST_SUITE_P(urEnqueueUSMMemcpy2DTestWithParam,
                 ::testing::ValuesIn(test_cases),
                 uur::print2DTestString<urEnqueueUSMMemcpy2DTestWithParam>);

TEST_P(urEnqueueUSMMemcpy2DTestWithParam, SuccessBlocking) {
    ASSERT_SUCCESS(urEnqueueUSMMemcpy2D(queue, true, pDst, pitch, pSrc, pitch,
                                        width, height, 0, nullptr, nullptr));
    ASSERT_NO_FATAL_FAILURE(verifyMemcpySucceeded());
}

TEST_P(urEnqueueUSMMemcpy2DTestWithParam, SuccessNonBlocking) {
    ur_event_handle_t memcpy_event = nullptr;
    ASSERT_SUCCESS(urEnqueueUSMMemcpy2D(queue, false, pDst, pitch, pSrc, pitch,
                                        width, height, 0, nullptr,
                                        &memcpy_event));
    ASSERT_SUCCESS(urQueueFlush(queue));
    ASSERT_SUCCESS(urEventWait(1, &memcpy_event));
    const auto eventStatus = uur::GetEventInfo<ur_event_status_t>(
        memcpy_event, UR_EVENT_INFO_COMMAND_EXECUTION_STATUS);
    ASSERT_TRUE(eventStatus.has_value());
    ASSERT_EQ(eventStatus.value(), UR_EVENT_STATUS_COMPLETE);

    ASSERT_NO_FATAL_FAILURE(verifyMemcpySucceeded());
}

using urEnqueueUSMMemcpy2DNegativeTest = urEnqueueUSMMemcpy2DTestWithParam;
UUR_TEST_SUITE_P(urEnqueueUSMMemcpy2DNegativeTest,
                 ::testing::Values(uur::TestParameters2D{1, 1, 1}),
                 uur::print2DTestString<urEnqueueUSMMemcpy2DTestWithParam>);

TEST_P(urEnqueueUSMMemcpy2DNegativeTest, InvalidNullHandleQueue) {
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_NULL_HANDLE,
                     urEnqueueUSMMemcpy2D(nullptr, true, pDst, pitch, pSrc,
                                          pitch, width, height, 0, nullptr,
                                          nullptr));
}

TEST_P(urEnqueueUSMMemcpy2DNegativeTest, InvalidNullPointer) {
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_NULL_POINTER,
                     urEnqueueUSMMemcpy2D(queue, true, nullptr, pitch, pSrc,
                                          pitch, width, height, 0, nullptr,
                                          nullptr));
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_NULL_POINTER,
                     urEnqueueUSMMemcpy2D(queue, true, pDst, pitch, nullptr,
                                          pitch, width, height, 0, nullptr,
                                          nullptr));
}

TEST_P(urEnqueueUSMMemcpy2DNegativeTest, InvalidSize) {
    // dstPitch == 0
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_SIZE,
                     urEnqueueUSMMemcpy2D(queue, true, pDst, 0, pSrc, pitch,
                                          width, height, 0, nullptr, nullptr));

    // srcPitch == 0
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_SIZE,
                     urEnqueueUSMMemcpy2D(queue, true, pDst, pitch, pSrc, 0,
                                          width, height, 0, nullptr, nullptr));

    // height == 0
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_SIZE,
                     urEnqueueUSMMemcpy2D(queue, true, pDst, pitch, pSrc, pitch,
                                          width, 0, 0, nullptr, nullptr));

    // dstPitch < width or srcPitch < width
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_SIZE,
                     urEnqueueUSMMemcpy2D(queue, true, pDst, pitch, pSrc, pitch,
                                          width + 1, height, 0, nullptr,
                                          nullptr));

    // `dstPitch * height` is higher than the allocation size of `pDst`
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_SIZE,
                     urEnqueueUSMMemcpy2D(queue, true, pDst, pitch + 1, pSrc,
                                          pitch, width, height, 0, nullptr,
                                          nullptr));

    // `srcPitch * height` is higher than the allocation size of `pSrc`
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_SIZE,
                     urEnqueueUSMMemcpy2D(queue, true, pDst, pitch, pSrc,
                                          pitch + 1, width, height, 0, nullptr,
                                          nullptr));
}

TEST_P(urEnqueueUSMMemcpy2DNegativeTest, InvalidEventWaitList) {
    // enqueue something to get an event
    ur_event_handle_t event = nullptr;
    int fill_pattern = 14;
    ASSERT_SUCCESS(urEnqueueUSMFill2D(queue, pDst, pitch, sizeof(fill_pattern),
                                      &fill_pattern, width, height, 0, nullptr,
                                      &event));
    ASSERT_NE(event, nullptr);
    ASSERT_SUCCESS(urQueueFinish(queue));

    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST,
                     urEnqueueUSMMemcpy2D(queue, true, pDst, pitch, pSrc, pitch,
                                          width, height, 1, nullptr, nullptr));
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST,
                     urEnqueueUSMMemcpy2D(queue, true, pDst, pitch, pSrc, pitch,
                                          width, height, 0, &event, nullptr));

    ASSERT_SUCCESS(urEventRelease(event));
}
