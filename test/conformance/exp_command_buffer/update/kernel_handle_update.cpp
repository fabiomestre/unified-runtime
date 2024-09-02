// Copyright (C) 2024 Intel Corporation
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "../fixtures.h"
#include <cstring>

//TODO

struct TestKernel {

  TestKernel(std::string Name, ur_platform_handle_t Platform, ur_context_handle_t Context, ur_device_handle_t Device)
      : Name(std::move(Name)), Platform(Platform), Context(Context), Device(Device) {

  }

  virtual ~TestKernel() = default;

  virtual void BuildKernel() {

    std::shared_ptr<std::vector<char>> ILBinary;
    std::vector<ur_program_metadata_t> Metadatas{};

    ur_platform_backend_t backend;
    ASSERT_SUCCESS(urPlatformGetInfo(Platform, UR_PLATFORM_INFO_BACKEND,
                                     sizeof(backend), &backend, nullptr));

    ASSERT_NO_FATAL_FAILURE(
        uur::KernelsEnvironment::instance->LoadSource(Name,
                                                      ILBinary));

    const ur_program_properties_t properties = {
        UR_STRUCTURE_TYPE_PROGRAM_PROPERTIES, nullptr,
        static_cast<uint32_t>(Metadatas.size()),
        Metadatas.empty() ? nullptr : Metadatas.data()};
    ASSERT_SUCCESS(uur::KernelsEnvironment::instance->CreateProgram(
        Platform, Context, Device, *ILBinary, &properties, &Program));

    auto KernelNames =
        uur::KernelsEnvironment::instance->GetEntryPointNames(Name);
    std::string KernelName = KernelNames[0];
    ASSERT_FALSE(KernelName.empty());

    ASSERT_SUCCESS(urProgramBuild(Context, Program, nullptr));
    ASSERT_SUCCESS(urKernelCreate(Program, KernelName.data(), &Kernel));
  }

  virtual void SetUpKernel() = 0;

  virtual void DestroyKernel() {
    ASSERT_SUCCESS(urKernelRelease(Kernel));
    ASSERT_SUCCESS(urProgramRelease(Program));
  };

  virtual void Validate() = 0;

  std::string Name;
  ur_platform_handle_t Platform;
  ur_context_handle_t Context;
  ur_device_handle_t Device;
  ur_program_handle_t Program;
  ur_kernel_handle_t Kernel;

};

struct TestSaxpyKernel : public TestKernel {

  TestSaxpyKernel(ur_platform_handle_t Platform, ur_context_handle_t Context, ur_device_handle_t Device)
      : TestKernel("saxpy_usm", Platform, Context,
                   Device) {}

  ~TestSaxpyKernel() override = default;

  void SetUpKernel() override {

    ASSERT_NO_FATAL_FAILURE(BuildKernel());

    const size_t allocation_size = sizeof(uint32_t) * global_size;
    for (auto &shared_ptr : shared_ptrs) {
      ASSERT_SUCCESS(urUSMSharedAlloc(Context, Device, nullptr, nullptr,
                                      allocation_size, &shared_ptr));
      ASSERT_NE(shared_ptr, nullptr);

      std::vector<uint8_t> pattern(allocation_size);
      uur::generateMemFillPattern(pattern);
      std::memcpy(shared_ptr, pattern.data(), allocation_size);
    }

    // Index 0 is output
    ASSERT_SUCCESS(
        urKernelSetArgPointer(Kernel, 0, nullptr, shared_ptrs[0]));
    // Index 1 is A
    ASSERT_SUCCESS(urKernelSetArgValue(Kernel, 1, sizeof(A), nullptr, &A));
    // Index 2 is X
    ASSERT_SUCCESS(
        urKernelSetArgPointer(Kernel, 2, nullptr, shared_ptrs[1]));
    // Index 3 is Y
    ASSERT_SUCCESS(
        urKernelSetArgPointer(Kernel, 3, nullptr, shared_ptrs[2]));
  }

  void DestroyKernel() override {
    for (auto &shared_ptr : shared_ptrs) {
      if (shared_ptr) {
        EXPECT_SUCCESS(urUSMFree(Context, shared_ptr));
      }
    }
    ASSERT_NO_FATAL_FAILURE(TestKernel::DestroyKernel());
  }

  void Validate() override {
    // TODO Test that no fatal failure works when the validation fails
    for (size_t i = 0; i < global_size; i++) {
      uint32_t result = A * X[i] + Y[i];
      ASSERT_EQ(result, output[i]);
    }
  }

  const size_t local_size = 4;
  const size_t global_size = 32;
  const size_t global_offset = 0;
  const size_t n_dimensions = 1;
  const uint32_t A = 42;

  std::array<void *, 5> shared_ptrs = {nullptr, nullptr, nullptr, nullptr};
  uint32_t *output = (uint32_t *) shared_ptrs[0];
  uint32_t *X = (uint32_t *) shared_ptrs[1];
  uint32_t *Y = (uint32_t *) shared_ptrs[2];
};

struct TestFill2DKernel : public TestKernel {

  TestFill2DKernel(ur_platform_handle_t Platform, ur_context_handle_t Context, ur_device_handle_t Device)
      : TestKernel("fill_usm_2d", Platform, Context,
                   Device) {}

  ~TestFill2DKernel() override = default;

  void SetUpKernel() override {
    ASSERT_NO_FATAL_FAILURE(BuildKernel());

    const size_t allocation_size = sizeof(uint32_t) * global_size;
    ASSERT_SUCCESS(urUSMSharedAlloc(Context, Device, nullptr, nullptr,
                                    allocation_size, &Memory));
    ASSERT_NE(Memory, nullptr);

    std::vector<uint8_t> pattern(allocation_size);
    uur::generateMemFillPattern(pattern);
    std::memcpy(Memory, pattern.data(), allocation_size);
  }

  void DestroyKernel() override {

    if (Memory) {
      EXPECT_SUCCESS(urUSMFree(Context, Memory));
    }

    ASSERT_NO_FATAL_FAILURE(TestKernel::DestroyKernel());
  }

  void Validate() override {
    for (size_t i = 0; i < global_size; i++) {
      ASSERT_EQ(static_cast<uint32_t *>(Memory)[i], Val);
    }
  }

  size_t local_size = 4;
  const size_t size_x = 64;
  const size_t size_y = 64;
  size_t global_size = size_x * size_y;
  size_t global_offset = 0;
  const size_t n_dimensions = 2;

  void *Memory;
  const uint32_t Val = 42;
};

struct KernelHandleUpdateTestBase
    : uur::command_buffer::urUpdatableCommandBufferExpTest {
  virtual void SetUp() override {

    UUR_RETURN_ON_FATAL_FAILURE(
        urUpdatableCommandBufferExpTest::SetUp());

    ur_device_usm_access_capability_flags_t shared_usm_flags;
    ASSERT_SUCCESS(
        uur::GetDeviceUSMSingleSharedSupport(device, shared_usm_flags));
    if (!(shared_usm_flags & UR_DEVICE_USM_ACCESS_CAPABILITY_FLAG_ACCESS)) {
      GTEST_SKIP() << "Shared USM is not supported.";
    }

    SaxpyKernel = std::make_shared<TestSaxpyKernel>(TestSaxpyKernel(platform, context, device));
    FillUSM2DKernel = std::make_shared<TestFill2DKernel>(TestFill2DKernel(platform, context, device));
    TestKernels.push_back(SaxpyKernel);
    TestKernels.push_back(FillUSM2DKernel);

    for (auto &TestKernel : TestKernels) {
      UUR_RETURN_ON_FATAL_FAILURE(TestKernel->SetUpKernel());
    }
  }

  virtual void TearDown() override {

    for (auto &TestKernel : TestKernels) {
      UUR_RETURN_ON_FATAL_FAILURE(TestKernel->DestroyKernel());
    }

    UUR_RETURN_ON_FATAL_FAILURE(
        urUpdatableCommandBufferExpTest::TearDown());
  }

  std::vector<std::shared_ptr<TestKernel>> TestKernels{};
  std::shared_ptr<TestSaxpyKernel> SaxpyKernel;
  std::shared_ptr<TestFill2DKernel> FillUSM2DKernel;
};

UUR_INSTANTIATE_DEVICE_TEST_SUITE_P(KernelHandleUpdateTestBase);

TEST_P(KernelHandleUpdateTestBase, KernelHandleUpdateTest) {

  std::vector<ur_kernel_handle_t> KernelAlternatives = {FillUSM2DKernel->Kernel};

  ur_exp_command_buffer_command_handle_t command_handle;
  ASSERT_SUCCESS(urCommandBufferAppendKernelLaunchExp(
      updatable_cmd_buf_handle,
      SaxpyKernel->Kernel,
      SaxpyKernel->n_dimensions,
      &(SaxpyKernel->global_offset),
      &(SaxpyKernel->global_size),
      &(SaxpyKernel->local_size),
      KernelAlternatives.size(),
      KernelAlternatives.data(),
      0,
      nullptr,
      nullptr,
      &command_handle));
  ASSERT_NE(command_handle, nullptr);

  ASSERT_SUCCESS(urCommandBufferFinalizeExp(updatable_cmd_buf_handle));

  ASSERT_SUCCESS(urCommandBufferEnqueueExp(updatable_cmd_buf_handle, queue, 0,
                                           nullptr, nullptr));
  ASSERT_SUCCESS(urQueueFinish(queue));
  ASSERT_NO_FATAL_FAILURE(SaxpyKernel->Validate());

  ur_exp_command_buffer_update_pointer_arg_desc_t new_input_descs[2];

  new_input_descs[0] = {
      UR_STRUCTURE_TYPE_EXP_COMMAND_BUFFER_UPDATE_POINTER_ARG_DESC, // stype
      nullptr,                                                      // pNext
      0,               // argIndex
      nullptr,         // pProperties
      &FillUSM2DKernel->Memory, // pArgValue
  };

  uint32_t new_A = 33;
  ur_exp_command_buffer_update_value_arg_desc_t new_A_desc = {
      UR_STRUCTURE_TYPE_EXP_COMMAND_BUFFER_UPDATE_VALUE_ARG_DESC, // stype
      nullptr,                                                    // pNext
      1,                                                          // argIndex
      sizeof(new_A),                                              // argSize
      nullptr, // pProperties
      &FillUSM2DKernel->Val,  // hArgValue
  };

  // Update kernel inputs
  ur_exp_command_buffer_update_kernel_launch_desc_t update_desc = {
      UR_STRUCTURE_TYPE_EXP_COMMAND_BUFFER_UPDATE_KERNEL_LAUNCH_DESC, // stype
      nullptr,                                                        // pNext
      FillUSM2DKernel->Kernel,
      0,               // numNewMemObjArgs
      1,               // numNewPointerArgs
      1,               // numNewValueArgs
      2,               // newWorkDim
      nullptr,         // pNewMemObjArgList
      new_input_descs, // pNewPointerArgList
      &new_A_desc,     // pNewValueArgList
      &FillUSM2DKernel->global_offset,       // pNewGlobalWorkOffset
      &FillUSM2DKernel->global_size,         // pNewGlobalWorkSize
      &FillUSM2DKernel->local_size,          // pNewLocalWorkSize
  };

  ASSERT_SUCCESS(
      urCommandBufferUpdateKernelLaunchExp(command_handle, &update_desc));
  ASSERT_SUCCESS(urCommandBufferEnqueueExp(updatable_cmd_buf_handle, queue, 0,
                                           nullptr, nullptr));
  ASSERT_SUCCESS(urQueueFinish(queue));

  ASSERT_NO_FATAL_FAILURE(FillUSM2DKernel->Validate());
}
