// Copyright (C) 2024 Intel Corporation
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "../fixtures.h"
#include "uur/raii.h"
#include <cstring>

// Tests that it is possible to update the kernel handle of a command-buffer node.
// This test launches a Saxpy kernel using a command-buffer and then updates the
// node with a completely different kernel that does a fill 2D operation.
struct TestKernel {

    TestKernel(std::string Name, ur_platform_handle_t Platform,
               ur_context_handle_t Context, ur_device_handle_t Device)
        : Name(std::move(Name)), Platform(Platform), Context(Context),
          Device(Device) {}

    virtual ~TestKernel() = default;

    virtual void buildKernel() {
        std::shared_ptr<std::vector<char>> ILBinary;
        std::vector<ur_program_metadata_t> Metadatas{};

        ur_platform_backend_t Backend;
        ASSERT_SUCCESS(urPlatformGetInfo(Platform, UR_PLATFORM_INFO_BACKEND,
                                         sizeof(Backend), &Backend, nullptr));

        ASSERT_NO_FATAL_FAILURE(
            uur::KernelsEnvironment::instance->LoadSource(Name, ILBinary));

        const ur_program_properties_t Properties = {
            UR_STRUCTURE_TYPE_PROGRAM_PROPERTIES, nullptr,
            static_cast<uint32_t>(Metadatas.size()),
            Metadatas.empty() ? nullptr : Metadatas.data()};
        ASSERT_SUCCESS(uur::KernelsEnvironment::instance->CreateProgram(
            Platform, Context, Device, *ILBinary, &Properties, &Program));

        auto KernelNames =
            uur::KernelsEnvironment::instance->GetEntryPointNames(Name);
        std::string KernelName = KernelNames[0];
        ASSERT_FALSE(KernelName.empty());

        ASSERT_SUCCESS(urProgramBuild(Context, Program, nullptr));
        ASSERT_SUCCESS(urKernelCreate(Program, KernelName.data(), &Kernel));
    }

    virtual void setUpKernel() = 0;

    virtual void destroyKernel() {
        ASSERT_SUCCESS(urKernelRelease(Kernel));
        ASSERT_SUCCESS(urProgramRelease(Program));
    };

    virtual void validate() = 0;

    std::string Name;
    ur_platform_handle_t Platform;
    ur_context_handle_t Context;
    ur_device_handle_t Device;
    ur_program_handle_t Program;
    ur_kernel_handle_t Kernel;
};

struct TestSaxpyKernel : public TestKernel {

    TestSaxpyKernel(ur_platform_handle_t Platform, ur_context_handle_t Context,
                    ur_device_handle_t Device)
        : TestKernel("saxpy_usm", Platform, Context, Device) {}

    ~TestSaxpyKernel() override = default;

    void setUpKernel() override {

        ASSERT_NO_FATAL_FAILURE(buildKernel());

        const size_t AllocationSize = sizeof(uint32_t) * GlobalSize;
        for (auto &SharedPtr : Memory) {
            ASSERT_SUCCESS(urUSMSharedAlloc(Context, Device, nullptr, nullptr,
                                            AllocationSize, &SharedPtr));
            ASSERT_NE(SharedPtr, nullptr);

            std::vector<uint8_t> pattern(AllocationSize);
            uur::generateMemFillPattern(pattern);
            std::memcpy(SharedPtr, pattern.data(), AllocationSize);
        }

        // Index 0 is the output
        ASSERT_SUCCESS(urKernelSetArgPointer(Kernel, 0, nullptr, Memory[0]));
        // Index 1 is A
        ASSERT_SUCCESS(urKernelSetArgValue(Kernel, 1, sizeof(A), nullptr, &A));
        // Index 2 is X
        ASSERT_SUCCESS(urKernelSetArgPointer(Kernel, 2, nullptr, Memory[1]));
        // Index 3 is Y
        ASSERT_SUCCESS(urKernelSetArgPointer(Kernel, 3, nullptr, Memory[2]));
    }

    void destroyKernel() override {
        for (auto &shared_ptr : Memory) {
            if (shared_ptr) {
                EXPECT_SUCCESS(urUSMFree(Context, shared_ptr));
            }
        }
        ASSERT_NO_FATAL_FAILURE(TestKernel::destroyKernel());
    }

    void validate() override {
        auto *output = static_cast<uint32_t *>(Memory[0]);
        auto *X = static_cast<uint32_t *>(Memory[1]);
        auto *Y = static_cast<uint32_t *>(Memory[2]);

        for (size_t i = 0; i < GlobalSize; i++) {
            uint32_t result = A * X[i] + Y[i];
            ASSERT_EQ(result, output[i]);
        }
    }

    const size_t LocalSize = 4;
    const size_t GlobalSize = 32;
    const size_t GlobalOffset = 0;
    const size_t NDimensions = 1;
    const uint32_t A = 42;

    std::array<void *, 3> Memory = {nullptr, nullptr, nullptr};
};

struct TestFill2DKernel : public TestKernel {

    TestFill2DKernel(ur_platform_handle_t Platform, ur_context_handle_t Context,
                     ur_device_handle_t Device)
        : TestKernel("fill_usm_2d", Platform, Context, Device) {}

    ~TestFill2DKernel() override = default;

    void setUpKernel() override {
        ASSERT_NO_FATAL_FAILURE(buildKernel());

        const size_t allocation_size = sizeof(uint32_t) * SizeX * SizeY;
        ASSERT_SUCCESS(urUSMSharedAlloc(Context, Device, nullptr, nullptr,
                                        allocation_size, &Memory));
        ASSERT_NE(Memory, nullptr);

        std::vector<uint8_t> pattern(allocation_size);
        uur::generateMemFillPattern(pattern);
        std::memcpy(Memory, pattern.data(), allocation_size);

        UpdatePointerDesc = {
            UR_STRUCTURE_TYPE_EXP_COMMAND_BUFFER_UPDATE_POINTER_ARG_DESC, // stype
            nullptr, // pNext
            0,       // argIndex
            nullptr, // pProperties
            &Memory, // pArgValue
        };

        UpdateValDesc = {
            UR_STRUCTURE_TYPE_EXP_COMMAND_BUFFER_UPDATE_VALUE_ARG_DESC, // stype
            nullptr,                                                    // pNext
            1,           // argIndex
            sizeof(Val), // argSize
            nullptr,     // pProperties
            &Val,        // hArgValue
        };

        UpdateDesc = {
            UR_STRUCTURE_TYPE_EXP_COMMAND_BUFFER_UPDATE_KERNEL_LAUNCH_DESC, // stype
            nullptr,             // pNext
            Kernel,              // hNewKernel
            0,                   // numNewMemObjArgs
            1,                   // numNewPointerArgs
            1,                   // numNewValueArgs
            NDimensions,         // newWorkDim
            nullptr,             // pNewMemObjArgList
            &UpdatePointerDesc,  // pNewPointerArgList
            &UpdateValDesc,      // pNewValueArgList
            GlobalOffset.data(), // pNewGlobalWorkOffset
            GlobalSize.data(),   // pNewGlobalWorkSize
            LocalSize.data(),    // pNewLocalWorkSize
        };
    }

    void destroyKernel() override {
        if (Memory) {
            EXPECT_SUCCESS(urUSMFree(Context, Memory));
        }
        ASSERT_NO_FATAL_FAILURE(TestKernel::destroyKernel());
    }

    void validate() override {
        for (size_t i = 0; i < SizeX * SizeY; i++) {
            ASSERT_EQ(static_cast<uint32_t *>(Memory)[i], Val);
        }
    }

    ur_exp_command_buffer_update_pointer_arg_desc_t UpdatePointerDesc;
    ur_exp_command_buffer_update_value_arg_desc_t UpdateValDesc;
    ur_exp_command_buffer_update_kernel_launch_desc_t UpdateDesc;

    std::vector<size_t> LocalSize = {4, 4};
    const size_t SizeX = 64;
    const size_t SizeY = 64;
    std::vector<size_t> GlobalSize = {SizeX, SizeY};
    std::vector<size_t> GlobalOffset = {0, 0};
    uint32_t NDimensions = 2;

    void *Memory;
    uint32_t Val = 42;
};

struct urCommandBufferKernelHandleUpdateTest
    : uur::command_buffer::urUpdatableCommandBufferExpTest {
    virtual void SetUp() override {

        UUR_RETURN_ON_FATAL_FAILURE(urUpdatableCommandBufferExpTest::SetUp());

        ur_device_usm_access_capability_flags_t shared_usm_flags;
        ASSERT_SUCCESS(
            uur::GetDeviceUSMSingleSharedSupport(device, shared_usm_flags));
        if (!(shared_usm_flags & UR_DEVICE_USM_ACCESS_CAPABILITY_FLAG_ACCESS)) {
            GTEST_SKIP() << "Shared USM is not supported.";
        }

        SaxpyKernel = std::make_shared<TestSaxpyKernel>(
            TestSaxpyKernel(platform, context, device));
        FillUSM2DKernel = std::make_shared<TestFill2DKernel>(
            TestFill2DKernel(platform, context, device));
        TestKernels.push_back(SaxpyKernel);
        TestKernels.push_back(FillUSM2DKernel);

        for (auto &TestKernel : TestKernels) {
            UUR_RETURN_ON_FATAL_FAILURE(TestKernel->setUpKernel());
        }
    }

    virtual void TearDown() override {
        for (auto &TestKernel : TestKernels) {
            UUR_RETURN_ON_FATAL_FAILURE(TestKernel->destroyKernel());
        }
        UUR_RETURN_ON_FATAL_FAILURE(
            urUpdatableCommandBufferExpTest::TearDown());
    }

    std::vector<std::shared_ptr<TestKernel>> TestKernels{};
    std::shared_ptr<TestSaxpyKernel> SaxpyKernel;
    std::shared_ptr<TestFill2DKernel> FillUSM2DKernel;
};

UUR_INSTANTIATE_DEVICE_TEST_SUITE_P(urCommandBufferKernelHandleUpdateTest);

TEST_P(urCommandBufferKernelHandleUpdateTest, Success) {

    std::vector<ur_kernel_handle_t> KernelAlternatives = {
        FillUSM2DKernel->Kernel};

    uur::raii::CommandBufferCommand CommandHandle;
    ASSERT_SUCCESS(urCommandBufferAppendKernelLaunchExp(
        updatable_cmd_buf_handle, SaxpyKernel->Kernel, SaxpyKernel->NDimensions,
        &(SaxpyKernel->GlobalOffset), &(SaxpyKernel->GlobalSize),
        &(SaxpyKernel->LocalSize), KernelAlternatives.size(),
        KernelAlternatives.data(), 0, nullptr, nullptr, CommandHandle.ptr()));
    ASSERT_NE(CommandHandle, nullptr);

    ASSERT_SUCCESS(urCommandBufferFinalizeExp(updatable_cmd_buf_handle));

    ASSERT_SUCCESS(urCommandBufferEnqueueExp(updatable_cmd_buf_handle, queue, 0,
                                             nullptr, nullptr));
    ASSERT_SUCCESS(urCommandBufferUpdateKernelLaunchExp(
        CommandHandle, &FillUSM2DKernel->UpdateDesc));
    ASSERT_SUCCESS(urCommandBufferEnqueueExp(updatable_cmd_buf_handle, queue, 0,
                                             nullptr, nullptr));
    ASSERT_SUCCESS(urQueueFinish(queue));

    ASSERT_NO_FATAL_FAILURE(SaxpyKernel->validate());
    ASSERT_NO_FATAL_FAILURE(FillUSM2DKernel->validate());
}

/* Test that updates to the command kernel handle are stored in the command handle */
TEST_P(urCommandBufferKernelHandleUpdateTest, UpdateAgain) {

    std::vector<ur_kernel_handle_t> KernelAlternatives = {
        FillUSM2DKernel->Kernel};

    uur::raii::CommandBufferCommand CommandHandle;
    ASSERT_SUCCESS(urCommandBufferAppendKernelLaunchExp(
        updatable_cmd_buf_handle, SaxpyKernel->Kernel, SaxpyKernel->NDimensions,
        &(SaxpyKernel->GlobalOffset), &(SaxpyKernel->GlobalSize),
        &(SaxpyKernel->LocalSize), KernelAlternatives.size(),
        KernelAlternatives.data(), 0, nullptr, nullptr, CommandHandle.ptr()));
    ASSERT_NE(CommandHandle, nullptr);

    ASSERT_SUCCESS(urCommandBufferFinalizeExp(updatable_cmd_buf_handle));
    ASSERT_SUCCESS(urCommandBufferEnqueueExp(updatable_cmd_buf_handle, queue, 0,
                                             nullptr, nullptr));
    ASSERT_SUCCESS(urCommandBufferUpdateKernelLaunchExp(
        CommandHandle, &FillUSM2DKernel->UpdateDesc));
    ASSERT_SUCCESS(urCommandBufferEnqueueExp(updatable_cmd_buf_handle, queue, 0,
                                             nullptr, nullptr));
    ASSERT_SUCCESS(urQueueFinish(queue));

    ASSERT_NO_FATAL_FAILURE(SaxpyKernel->validate());
    ASSERT_NO_FATAL_FAILURE(FillUSM2DKernel->validate());

    // If the Kernel was not stored properly in the command, then this could potentially fail since
    // it would try to use the Saxpy kernel
    FillUSM2DKernel->Val = 78;
    ASSERT_SUCCESS(urCommandBufferUpdateKernelLaunchExp(
        CommandHandle, &FillUSM2DKernel->UpdateDesc));
    ASSERT_SUCCESS(urCommandBufferEnqueueExp(updatable_cmd_buf_handle, queue, 0,
                                             nullptr, nullptr));
    ASSERT_SUCCESS(urQueueFinish(queue));
    ASSERT_NO_FATAL_FAILURE(FillUSM2DKernel->validate());
}

TEST_P(urCommandBufferKernelHandleUpdateTest, KernelAlternativeNotRegistered) {

    uur::raii::CommandBufferCommand CommandHandle;
    ASSERT_SUCCESS(urCommandBufferAppendKernelLaunchExp(
        updatable_cmd_buf_handle, SaxpyKernel->Kernel, SaxpyKernel->NDimensions,
        &(SaxpyKernel->GlobalOffset), &(SaxpyKernel->GlobalSize),
        &(SaxpyKernel->LocalSize), 0, nullptr, 0, nullptr, nullptr,
        CommandHandle.ptr()));
    ASSERT_NE(CommandHandle, nullptr);

    ASSERT_SUCCESS(urCommandBufferFinalizeExp(updatable_cmd_buf_handle));

    ASSERT_SUCCESS(urCommandBufferEnqueueExp(updatable_cmd_buf_handle, queue, 0,
                                             nullptr, nullptr));

    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_VALUE,
                     urCommandBufferUpdateKernelLaunchExp(
                         CommandHandle, &FillUSM2DKernel->UpdateDesc));
}

TEST_P(urCommandBufferKernelHandleUpdateTest,
       RegisterInvalidKernelAlternative) {

    std::vector<ur_kernel_handle_t> KernelAlternatives = {SaxpyKernel->Kernel};

    ur_exp_command_buffer_command_handle_t CommandHandle;
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_VALUE,
                     urCommandBufferAppendKernelLaunchExp(
                         updatable_cmd_buf_handle, SaxpyKernel->Kernel,
                         SaxpyKernel->NDimensions, &(SaxpyKernel->GlobalOffset),
                         &(SaxpyKernel->GlobalSize), &(SaxpyKernel->LocalSize),
                         KernelAlternatives.size(), KernelAlternatives.data(),
                         0, nullptr, nullptr, &CommandHandle));
}
