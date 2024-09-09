//===--------- command_buffer.cpp - CUDA Adapter --------------------------===//
//
// Copyright (C) 2023 Intel Corporation
//
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "command_buffer.hpp"

#include "common.hpp"
#include "enqueue.hpp"
#include "event.hpp"
#include "kernel.hpp"
#include "memory.hpp"
#include "queue.hpp"

#include <cstring>

namespace {
ur_result_t
commandBufferReleaseInternal(ur_exp_command_buffer_handle_t CommandBuffer) {
  if (CommandBuffer->decrementInternalReferenceCount() != 0) {
    return UR_RESULT_SUCCESS;
  }

  delete CommandBuffer;
  return UR_RESULT_SUCCESS;
}

ur_result_t
commandHandleReleaseInternal(ur_exp_command_buffer_command_handle_t Command) {
  if (Command->decrementInternalReferenceCount() != 0) {
    return UR_RESULT_SUCCESS;
  }

  // Decrement parent command-buffer internal ref count
  commandBufferReleaseInternal(Command->CommandBuffer);

  delete Command;
  return UR_RESULT_SUCCESS;
}
} // end anonymous namespace

ur_exp_command_buffer_handle_t_::ur_exp_command_buffer_handle_t_(
    ur_context_handle_t Context, ur_device_handle_t Device, bool IsUpdatable)
    : Context(Context), Device(Device),
      IsUpdatable(IsUpdatable), CudaGraph{nullptr}, CudaGraphExec{nullptr},
      RefCountInternal{1}, RefCountExternal{1}, NextSyncPoint{0} {
  urContextRetain(Context);
  urDeviceRetain(Device);
}

/// The ur_exp_command_buffer_handle_t_ destructor releases
/// all the memory objects allocated for command_buffer managment
ur_exp_command_buffer_handle_t_::~ur_exp_command_buffer_handle_t_() {
  // Release the memory allocated to the Context stored in the command_buffer
  UR_TRACE(urContextRelease(Context));

  // Release the device
  UR_TRACE(urDeviceRelease(Device));

  // Release the memory allocated to the CudaGraph
  cuGraphDestroy(CudaGraph);

  // Release the memory allocated to the CudaGraphExec
  if (CudaGraphExec) {
    cuGraphExecDestroy(CudaGraphExec);
  }
}

ur_exp_command_buffer_command_handle_t_::
    ur_exp_command_buffer_command_handle_t_(
        ur_exp_command_buffer_handle_t CommandBuffer, ur_kernel_handle_t Kernel,
        CUgraphNode Node, CUDA_KERNEL_NODE_PARAMS Params, uint32_t WorkDim,
        const size_t *GlobalWorkOffsetPtr, const size_t *GlobalWorkSizePtr,
        const size_t *LocalWorkSizePtr, uint32_t NumKernelAlternatives,
        ur_kernel_handle_t *KernelAlternatives)
    : CommandBuffer(CommandBuffer), Kernel(Kernel), ValidKernelHandles(),
      Node(Node), Params(Params), WorkDim(WorkDim), RefCountInternal(1),
      RefCountExternal(1) {
  CommandBuffer->incrementInternalReferenceCount();

  const size_t CopySize = sizeof(size_t) * WorkDim;
  std::memcpy(GlobalWorkOffset, GlobalWorkOffsetPtr, CopySize);
  std::memcpy(GlobalWorkSize, GlobalWorkSizePtr, CopySize);
  // Local work size may be nullptr
  if (LocalWorkSizePtr) {
    std::memcpy(LocalWorkSize, LocalWorkSizePtr, CopySize);
  } else {
    std::memset(LocalWorkSize, 0, sizeof(size_t) * 3);
  }

  if (WorkDim < 3) {
    const size_t ZeroSize = sizeof(size_t) * (3 - WorkDim);
    std::memset(GlobalWorkOffset + WorkDim, 0, ZeroSize);
    std::memset(GlobalWorkSize + WorkDim, 0, ZeroSize);
  }

  /* Add the default Kernel as a valid kernel handle for this command */
  ValidKernelHandles.insert(Kernel);
  if (KernelAlternatives) {
    ValidKernelHandles.insert(KernelAlternatives,
                              KernelAlternatives + NumKernelAlternatives);
  }
}

/// Helper function for finding the Cuda Nodes associated with the
/// commands in a command-buffer, each event is pointed to by a sync-point in
/// the wait list.
///
/// @param[in] CommandBuffer to lookup the events from.
/// @param[in] NumSyncPointsInWaitList Length of \p SyncPointWaitList.
/// @param[in] SyncPointWaitList List of sync points in \p CommandBuffer
/// to find the events for.
/// @param[out] CuNodesList Return parameter for the Cuda Nodes associated with
/// each sync-point in \p SyncPointWaitList.
///
/// @return UR_RESULT_SUCCESS or an error code on failure
static ur_result_t getNodesFromSyncPoints(
    const ur_exp_command_buffer_handle_t &CommandBuffer,
    size_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    std::vector<CUgraphNode> &CuNodesList) {
  // Map of ur_exp_command_buffer_sync_point_t to ur_event_handle_t defining
  // the event associated with each sync-point
  auto SyncPoints = CommandBuffer->SyncPoints;

  // For each sync-point add associated CUDA graph node to the return list.
  for (size_t i = 0; i < NumSyncPointsInWaitList; i++) {
    if (auto NodeHandle = SyncPoints.find(SyncPointWaitList[i]);
        NodeHandle != SyncPoints.end()) {
      CuNodesList.push_back(NodeHandle->second);
    } else {
      return UR_RESULT_ERROR_INVALID_VALUE;
    }
  }
  return UR_RESULT_SUCCESS;
}

/// Set parameter for General 1D memory copy.
/// If the source and/or destination is on the device, SrcPtr and/or DstPtr
/// must be a pointer to a CUdeviceptr
static void setCopyParams(const void *SrcPtr, const CUmemorytype_enum SrcType,
                          void *DstPtr, const CUmemorytype_enum DstType,
                          size_t Size, CUDA_MEMCPY3D &Params) {
  // Set all params to 0 first
  std::memset(&Params, 0, sizeof(CUDA_MEMCPY3D));

  Params.srcMemoryType = SrcType;
  Params.srcDevice = SrcType == CU_MEMORYTYPE_DEVICE
                         ? *static_cast<const CUdeviceptr *>(SrcPtr)
                         : 0;
  Params.srcHost = SrcType == CU_MEMORYTYPE_HOST ? SrcPtr : nullptr;
  Params.dstMemoryType = DstType;
  Params.dstDevice =
      DstType == CU_MEMORYTYPE_DEVICE ? *static_cast<CUdeviceptr *>(DstPtr) : 0;
  Params.dstHost = DstType == CU_MEMORYTYPE_HOST ? DstPtr : nullptr;
  Params.WidthInBytes = Size;
  Params.Height = 1;
  Params.Depth = 1;
}

// Helper function for enqueuing memory fills
static ur_result_t enqueueCommandBufferFillHelper(
    ur_exp_command_buffer_handle_t CommandBuffer, void *DstDevice,
    const CUmemorytype_enum DstType, const void *Pattern, size_t PatternSize,
    size_t Size, uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *RetSyncPoint) {
  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(CommandBuffer, NumSyncPointsInWaitList,
                                        SyncPointWaitList, DepsList));

  try {
    // Graph node added to graph, if multiple nodes are created this will
    // be set to the leaf node
    CUgraphNode GraphNode;

    const size_t N = Size / PatternSize;
    auto DstPtr = DstType == CU_MEMORYTYPE_DEVICE
                      ? *static_cast<CUdeviceptr *>(DstDevice)
                      : (CUdeviceptr)DstDevice;

    if ((PatternSize == 1) || (PatternSize == 2) || (PatternSize == 4)) {
      CUDA_MEMSET_NODE_PARAMS NodeParams = {};
      NodeParams.dst = DstPtr;
      NodeParams.elementSize = PatternSize;
      NodeParams.height = N;
      NodeParams.pitch = PatternSize;
      NodeParams.width = 1;

      // pattern size in bytes
      switch (PatternSize) {
      case 1: {
        auto Value = *static_cast<const uint8_t *>(Pattern);
        NodeParams.value = Value;
        break;
      }
      case 2: {
        auto Value = *static_cast<const uint16_t *>(Pattern);
        NodeParams.value = Value;
        break;
      }
      case 4: {
        auto Value = *static_cast<const uint32_t *>(Pattern);
        NodeParams.value = Value;
        break;
      }
      }

      UR_CHECK_ERROR(
          cuGraphAddMemsetNode(&GraphNode, CommandBuffer->CudaGraph,
                               DepsList.data(), DepsList.size(), &NodeParams,
                               CommandBuffer->Device->getNativeContext()));
    } else {
      // CUDA has no memset functions that allow setting values more than 4
      // bytes. UR API lets you pass an arbitrary "pattern" to the buffer
      // fill, which can be more than 4 bytes. We must break up the pattern
      // into 1 byte values, and set the buffer using multiple strided calls.
      // This means that one cuGraphAddMemsetNode call is made for every 1
      // bytes in the pattern.

      size_t NumberOfSteps = PatternSize / sizeof(uint8_t);

      // Update NodeParam
      CUDA_MEMSET_NODE_PARAMS NodeParamsStepFirst = {};
      NodeParamsStepFirst.dst = DstPtr;
      NodeParamsStepFirst.elementSize = sizeof(uint32_t);
      NodeParamsStepFirst.height = Size / sizeof(uint32_t);
      NodeParamsStepFirst.pitch = sizeof(uint32_t);
      NodeParamsStepFirst.value = *static_cast<const uint32_t *>(Pattern);
      NodeParamsStepFirst.width = 1;

      UR_CHECK_ERROR(cuGraphAddMemsetNode(
          &GraphNode, CommandBuffer->CudaGraph, DepsList.data(),
          DepsList.size(), &NodeParamsStepFirst,
          CommandBuffer->Device->getNativeContext()));

      DepsList.clear();
      DepsList.push_back(GraphNode);

      // we walk up the pattern in 1-byte steps, and call cuMemset for each
      // 1-byte chunk of the pattern.
      for (auto Step = 4u; Step < NumberOfSteps; ++Step) {
        // take 4 bytes of the pattern
        auto Value = *(static_cast<const uint8_t *>(Pattern) + Step);

        // offset the pointer to the part of the buffer we want to write to
        auto OffsetPtr = DstPtr + (Step * sizeof(uint8_t));

        // Update NodeParam
        CUDA_MEMSET_NODE_PARAMS NodeParamsStep = {};
        NodeParamsStep.dst = (CUdeviceptr)OffsetPtr;
        NodeParamsStep.elementSize = sizeof(uint8_t);
        NodeParamsStep.height = Size / NumberOfSteps;
        NodeParamsStep.pitch = NumberOfSteps * sizeof(uint8_t);
        NodeParamsStep.value = Value;
        NodeParamsStep.width = 1;

        UR_CHECK_ERROR(cuGraphAddMemsetNode(
            &GraphNode, CommandBuffer->CudaGraph, DepsList.data(),
            DepsList.size(), &NodeParamsStep,
            CommandBuffer->Device->getNativeContext()));

        DepsList.clear();
        DepsList.push_back(GraphNode);
      }
    }

    // Get sync point and register the cuNode with it.
    auto SyncPoint = CommandBuffer->addSyncPoint(GraphNode);
    if (RetSyncPoint) {
      *RetSyncPoint = SyncPoint;
    }
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferCreateExp(
    ur_context_handle_t hContext, ur_device_handle_t hDevice,
    const ur_exp_command_buffer_desc_t *pCommandBufferDesc,
    ur_exp_command_buffer_handle_t *phCommandBuffer) {

  const bool IsUpdatable =
      pCommandBufferDesc ? pCommandBufferDesc->isUpdatable : false;

  try {
    *phCommandBuffer =
        new ur_exp_command_buffer_handle_t_(hContext, hDevice, IsUpdatable);
  } catch (const std::bad_alloc &) {
    return UR_RESULT_ERROR_OUT_OF_HOST_MEMORY;
  } catch (...) {
    return UR_RESULT_ERROR_UNKNOWN;
  }

  try {
    UR_CHECK_ERROR(cuGraphCreate(&(*phCommandBuffer)->CudaGraph, 0));
  } catch (...) {
    return UR_RESULT_ERROR_OUT_OF_RESOURCES;
  }

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferRetainExp(ur_exp_command_buffer_handle_t hCommandBuffer) {
  hCommandBuffer->incrementInternalReferenceCount();
  hCommandBuffer->incrementExternalReferenceCount();
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferReleaseExp(ur_exp_command_buffer_handle_t hCommandBuffer) {
  if (hCommandBuffer->decrementExternalReferenceCount() == 0) {
    // External ref count has reached zero, internal release of created
    // commands.
    for (auto Command : hCommandBuffer->CommandHandles) {
      commandHandleReleaseInternal(Command);
    }
  }

  return commandBufferReleaseInternal(hCommandBuffer);
}

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferFinalizeExp(ur_exp_command_buffer_handle_t hCommandBuffer) {
  try {
    const unsigned long long flags = 0;
#if CUDA_VERSION >= 12000
    UR_CHECK_ERROR(cuGraphInstantiate(&hCommandBuffer->CudaGraphExec,
                                      hCommandBuffer->CudaGraph, flags));
#elif CUDA_VERSION >= 11040
    UR_CHECK_ERROR(cuGraphInstantiateWithFlags(
        &hCommandBuffer->CudaGraphExec, hCommandBuffer->CudaGraph, flags));
#else
    // Cannot use flags
    UR_CHECK_ERROR(cuGraphInstantiate(&hCommandBuffer->CudaGraphExec,
                                      hCommandBuffer->CudaGraph, nullptr,
                                      nullptr, 0));
#endif
  } catch (...) {
    return UR_RESULT_ERROR_UNKNOWN;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendKernelLaunchExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_kernel_handle_t hKernel,
    uint32_t workDim, const size_t *pGlobalWorkOffset,
    const size_t *pGlobalWorkSize, const size_t *pLocalWorkSize,
    uint32_t numKernelAlternatives, ur_kernel_handle_t *phKernelAlternatives,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint,
    ur_exp_command_buffer_command_handle_t *phCommand) {
  // Preconditions
  UR_ASSERT(hCommandBuffer->Context == hKernel->getContext(),
            UR_RESULT_ERROR_INVALID_KERNEL);
  UR_ASSERT(workDim > 0, UR_RESULT_ERROR_INVALID_WORK_DIMENSION);
  UR_ASSERT(workDim < 4, UR_RESULT_ERROR_INVALID_WORK_DIMENSION);

  for (uint32_t i = 0; i < numKernelAlternatives; ++i) {
    UR_ASSERT(phKernelAlternatives[i] != hKernel,
              UR_RESULT_ERROR_INVALID_VALUE);
  }

  CUgraphNode GraphNode;

  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  if (*pGlobalWorkSize == 0) {
    try {
      // Create an empty node if the kernel workload size is zero
      UR_CHECK_ERROR(cuGraphAddEmptyNode(&GraphNode, hCommandBuffer->CudaGraph,
                                         DepsList.data(), DepsList.size()));

      // Get sync point and register the cuNode with it.
      auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
      if (pSyncPoint) {
        *pSyncPoint = SyncPoint;
      }
    } catch (ur_result_t Err) {
      return Err;
    }
    return UR_RESULT_SUCCESS;
  }

  // Set the number of threads per block to the number of threads per warp
  // by default unless user has provided a better number
  size_t ThreadsPerBlock[3] = {32u, 1u, 1u};
  size_t BlocksPerGrid[3] = {1u, 1u, 1u};

  uint32_t LocalSize = hKernel->getLocalSize();
  CUfunction CuFunc = hKernel->get();
  UR_CHECK_ERROR(
      setKernelParams(hCommandBuffer->Context, hCommandBuffer->Device, workDim,
                      pGlobalWorkOffset, pGlobalWorkSize, pLocalWorkSize,
                      hKernel, CuFunc, ThreadsPerBlock, BlocksPerGrid));

  try {
    // Set node param structure with the kernel related data
    auto &ArgIndices = hKernel->getArgIndices();
    CUDA_KERNEL_NODE_PARAMS NodeParams = {};
    NodeParams.func = CuFunc;
    NodeParams.gridDimX = BlocksPerGrid[0];
    NodeParams.gridDimY = BlocksPerGrid[1];
    NodeParams.gridDimZ = BlocksPerGrid[2];
    NodeParams.blockDimX = ThreadsPerBlock[0];
    NodeParams.blockDimY = ThreadsPerBlock[1];
    NodeParams.blockDimZ = ThreadsPerBlock[2];
    NodeParams.sharedMemBytes = LocalSize;
    NodeParams.kernelParams = const_cast<void **>(ArgIndices.data());

    // Create and add an new kernel node to the Cuda graph
    UR_CHECK_ERROR(cuGraphAddKernelNode(&GraphNode, hCommandBuffer->CudaGraph,
                                        DepsList.data(), DepsList.size(),
                                        &NodeParams));

    if (LocalSize != 0)
      hKernel->clearLocalSize();

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }

    auto NewCommand = new ur_exp_command_buffer_command_handle_t_{
        hCommandBuffer,      hKernel,        GraphNode,
        NodeParams,          workDim,        pGlobalWorkOffset,
        pGlobalWorkSize,     pLocalWorkSize, numKernelAlternatives,
        phKernelAlternatives};

    NewCommand->incrementInternalReferenceCount();
    hCommandBuffer->CommandHandles.push_back(NewCommand);

    if (phCommand) {
      *phCommand = NewCommand;
    }

  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMMemcpyExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, void *pDst, const void *pSrc,
    size_t size, uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  CUgraphNode GraphNode;
  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  try {
    CUDA_MEMCPY3D NodeParams = {};
    setCopyParams(pSrc, CU_MEMORYTYPE_HOST, pDst, CU_MEMORYTYPE_HOST, size,
                  NodeParams);

    UR_CHECK_ERROR(cuGraphAddMemcpyNode(
        &GraphNode, hCommandBuffer->CudaGraph, DepsList.data(), DepsList.size(),
        &NodeParams, hCommandBuffer->Device->getNativeContext()));

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferCopyExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hSrcMem,
    ur_mem_handle_t hDstMem, size_t srcOffset, size_t dstOffset, size_t size,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  CUgraphNode GraphNode;
  std::vector<CUgraphNode> DepsList;

  UR_ASSERT(size + dstOffset <= std::get<BufferMem>(hDstMem->Mem).getSize(),
            UR_RESULT_ERROR_INVALID_SIZE);
  UR_ASSERT(size + srcOffset <= std::get<BufferMem>(hSrcMem->Mem).getSize(),
            UR_RESULT_ERROR_INVALID_SIZE);

  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  try {
    auto Src = std::get<BufferMem>(hSrcMem->Mem)
                   .getPtrWithOffset(hCommandBuffer->Device, srcOffset);
    auto Dst = std::get<BufferMem>(hDstMem->Mem)
                   .getPtrWithOffset(hCommandBuffer->Device, dstOffset);

    CUDA_MEMCPY3D NodeParams = {};
    setCopyParams(&Src, CU_MEMORYTYPE_DEVICE, &Dst, CU_MEMORYTYPE_DEVICE, size,
                  NodeParams);

    UR_CHECK_ERROR(cuGraphAddMemcpyNode(
        &GraphNode, hCommandBuffer->CudaGraph, DepsList.data(), DepsList.size(),
        &NodeParams, hCommandBuffer->Device->getNativeContext()));

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferCopyRectExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hSrcMem,
    ur_mem_handle_t hDstMem, ur_rect_offset_t srcOrigin,
    ur_rect_offset_t dstOrigin, ur_rect_region_t region, size_t srcRowPitch,
    size_t srcSlicePitch, size_t dstRowPitch, size_t dstSlicePitch,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  CUgraphNode GraphNode;
  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  try {
    auto SrcPtr =
        std::get<BufferMem>(hSrcMem->Mem).getPtr(hCommandBuffer->Device);
    auto DstPtr =
        std::get<BufferMem>(hDstMem->Mem).getPtr(hCommandBuffer->Device);
    CUDA_MEMCPY3D NodeParams = {};

    setCopyRectParams(region, &SrcPtr, CU_MEMORYTYPE_DEVICE, srcOrigin,
                      srcRowPitch, srcSlicePitch, &DstPtr, CU_MEMORYTYPE_DEVICE,
                      dstOrigin, dstRowPitch, dstSlicePitch, NodeParams);

    UR_CHECK_ERROR(cuGraphAddMemcpyNode(
        &GraphNode, hCommandBuffer->CudaGraph, DepsList.data(), DepsList.size(),
        &NodeParams, hCommandBuffer->Device->getNativeContext()));

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT
ur_result_t UR_APICALL urCommandBufferAppendMemBufferWriteExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    size_t offset, size_t size, const void *pSrc,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  CUgraphNode GraphNode;
  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  try {
    auto Dst = std::get<BufferMem>(hBuffer->Mem)
                   .getPtrWithOffset(hCommandBuffer->Device, offset);

    CUDA_MEMCPY3D NodeParams = {};
    setCopyParams(pSrc, CU_MEMORYTYPE_HOST, &Dst, CU_MEMORYTYPE_DEVICE, size,
                  NodeParams);

    UR_CHECK_ERROR(cuGraphAddMemcpyNode(
        &GraphNode, hCommandBuffer->CudaGraph, DepsList.data(), DepsList.size(),
        &NodeParams, hCommandBuffer->Device->getNativeContext()));

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT
ur_result_t UR_APICALL urCommandBufferAppendMemBufferReadExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    size_t offset, size_t size, void *pDst, uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  CUgraphNode GraphNode;
  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  try {
    auto Src = std::get<BufferMem>(hBuffer->Mem)
                   .getPtrWithOffset(hCommandBuffer->Device, offset);

    CUDA_MEMCPY3D NodeParams = {};
    setCopyParams(&Src, CU_MEMORYTYPE_DEVICE, pDst, CU_MEMORYTYPE_HOST, size,
                  NodeParams);

    UR_CHECK_ERROR(cuGraphAddMemcpyNode(
        &GraphNode, hCommandBuffer->CudaGraph, DepsList.data(), DepsList.size(),
        &NodeParams, hCommandBuffer->Device->getNativeContext()));

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT
ur_result_t UR_APICALL urCommandBufferAppendMemBufferWriteRectExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    ur_rect_offset_t bufferOffset, ur_rect_offset_t hostOffset,
    ur_rect_region_t region, size_t bufferRowPitch, size_t bufferSlicePitch,
    size_t hostRowPitch, size_t hostSlicePitch, void *pSrc,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  CUgraphNode GraphNode;
  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  try {
    auto DstPtr =
        std::get<BufferMem>(hBuffer->Mem).getPtr(hCommandBuffer->Device);
    CUDA_MEMCPY3D NodeParams = {};

    setCopyRectParams(region, pSrc, CU_MEMORYTYPE_HOST, hostOffset,
                      hostRowPitch, hostSlicePitch, &DstPtr,
                      CU_MEMORYTYPE_DEVICE, bufferOffset, bufferRowPitch,
                      bufferSlicePitch, NodeParams);

    UR_CHECK_ERROR(cuGraphAddMemcpyNode(
        &GraphNode, hCommandBuffer->CudaGraph, DepsList.data(), DepsList.size(),
        &NodeParams, hCommandBuffer->Device->getNativeContext()));

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT
ur_result_t UR_APICALL urCommandBufferAppendMemBufferReadRectExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    ur_rect_offset_t bufferOffset, ur_rect_offset_t hostOffset,
    ur_rect_region_t region, size_t bufferRowPitch, size_t bufferSlicePitch,
    size_t hostRowPitch, size_t hostSlicePitch, void *pDst,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  CUgraphNode GraphNode;
  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  try {
    auto SrcPtr =
        std::get<BufferMem>(hBuffer->Mem).getPtr(hCommandBuffer->Device);
    CUDA_MEMCPY3D NodeParams = {};

    setCopyRectParams(region, &SrcPtr, CU_MEMORYTYPE_DEVICE, bufferOffset,
                      bufferRowPitch, bufferSlicePitch, pDst,
                      CU_MEMORYTYPE_HOST, hostOffset, hostRowPitch,
                      hostSlicePitch, NodeParams);

    UR_CHECK_ERROR(cuGraphAddMemcpyNode(
        &GraphNode, hCommandBuffer->CudaGraph, DepsList.data(), DepsList.size(),
        &NodeParams, hCommandBuffer->Device->getNativeContext()));

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMPrefetchExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, const void * /* Mem */,
    size_t /*Size*/, ur_usm_migration_flags_t /*Flags*/,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  // Prefetch cmd is not supported by Cuda Graph.
  // We implement it as an empty node to enforce dependencies.
  CUgraphNode GraphNode;

  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  try {
    // Add an empty node to preserve dependencies.
    UR_CHECK_ERROR(cuGraphAddEmptyNode(&GraphNode, hCommandBuffer->CudaGraph,
                                       DepsList.data(), DepsList.size()));

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }

  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMAdviseExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, const void * /* Mem */,
    size_t /*Size*/, ur_usm_advice_flags_t /*Advice*/,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  // Mem-Advise cmd is not supported by Cuda Graph.
  // We implement it as an empty node to enforce dependencies.
  CUgraphNode GraphNode;

  std::vector<CUgraphNode> DepsList;
  UR_CHECK_ERROR(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                        pSyncPointWaitList, DepsList));

  try {
    // Add an empty node to preserve dependencies.
    UR_CHECK_ERROR(cuGraphAddEmptyNode(&GraphNode, hCommandBuffer->CudaGraph,
                                       DepsList.data(), DepsList.size()));

    // Get sync point and register the cuNode with it.
    auto SyncPoint = hCommandBuffer->addSyncPoint(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = SyncPoint;
    }
  } catch (ur_result_t Err) {
    return Err;
  }

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferFillExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    const void *pPattern, size_t patternSize, size_t offset, size_t size,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  auto ArgsAreMultiplesOfPatternSize =
      (offset % patternSize == 0) || (size % patternSize == 0);

  auto PatternIsValid = (pPattern != nullptr);

  auto PatternSizeIsValid = ((patternSize & (patternSize - 1)) == 0) &&
                            (patternSize > 0); // is a positive power of two
  UR_ASSERT(ArgsAreMultiplesOfPatternSize && PatternIsValid &&
                PatternSizeIsValid,
            UR_RESULT_ERROR_INVALID_SIZE);

  auto DstDevice = std::get<BufferMem>(hBuffer->Mem)
                       .getPtrWithOffset(hCommandBuffer->Device, offset);

  return enqueueCommandBufferFillHelper(
      hCommandBuffer, &DstDevice, CU_MEMORYTYPE_DEVICE, pPattern, patternSize,
      size, numSyncPointsInWaitList, pSyncPointWaitList, pSyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMFillExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, void *pPtr,
    const void *pPattern, size_t patternSize, size_t size,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {

  auto PatternIsValid = (pPattern != nullptr);

  auto PatternSizeIsValid = ((patternSize & (patternSize - 1)) == 0) &&
                            (patternSize > 0); // is a positive power of two

  UR_ASSERT(PatternIsValid && PatternSizeIsValid, UR_RESULT_ERROR_INVALID_SIZE);
  return enqueueCommandBufferFillHelper(
      hCommandBuffer, pPtr, CU_MEMORYTYPE_UNIFIED, pPattern, patternSize, size,
      numSyncPointsInWaitList, pSyncPointWaitList, pSyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferEnqueueExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_queue_handle_t hQueue,
    uint32_t numEventsInWaitList, const ur_event_handle_t *phEventWaitList,
    ur_event_handle_t *phEvent) {

  try {
    std::unique_ptr<ur_event_handle_t_> RetImplEvent{nullptr};
    ScopedContext Active(hQueue->getDevice());
    uint32_t StreamToken;
    ur_stream_guard_ Guard;
    CUstream CuStream = hQueue->getNextComputeStream(
        numEventsInWaitList, phEventWaitList, Guard, &StreamToken);

    UR_CHECK_ERROR(enqueueEventsWait(hQueue, CuStream, numEventsInWaitList,
                                     phEventWaitList));

    if (phEvent) {
      RetImplEvent = std::unique_ptr<ur_event_handle_t_>(
          ur_event_handle_t_::makeNative(UR_COMMAND_COMMAND_BUFFER_ENQUEUE_EXP,
                                         hQueue, CuStream, StreamToken));
      UR_CHECK_ERROR(RetImplEvent->start());
    }

    // Launch graph
    UR_CHECK_ERROR(cuGraphLaunch(hCommandBuffer->CudaGraphExec, CuStream));

    if (phEvent) {
      UR_CHECK_ERROR(RetImplEvent->record());
      *phEvent = RetImplEvent.release();
    }
  } catch (ur_result_t Err) {
    return Err;
  }

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferRetainCommandExp(
    ur_exp_command_buffer_command_handle_t hCommand) {
  hCommand->incrementExternalReferenceCount();
  hCommand->incrementInternalReferenceCount();
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferReleaseCommandExp(
    ur_exp_command_buffer_command_handle_t hCommand) {
  hCommand->decrementExternalReferenceCount();
  return commandHandleReleaseInternal(hCommand);
}

/**
 * Validates contents of the update command description.
 * @param[in] Command The command which is being updated.
 * @param[in] UpdateCommandDesc The update command description.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t
validateCommandDesc(ur_exp_command_buffer_command_handle_t Command,
                    const ur_exp_command_buffer_update_kernel_launch_desc_t
                        *UpdateCommandDesc) {

  auto CommandBuffer = Command->CommandBuffer;

  // Update requires command-buffer to be finalized
  if (!CommandBuffer->CudaGraphExec) {
    return UR_RESULT_ERROR_INVALID_OPERATION;
  }

  // Update requires command-buffer to be created with update enabled
  if (!CommandBuffer->IsUpdatable) {
    return UR_RESULT_ERROR_INVALID_OPERATION;
  }

  const uint32_t NewWorkDim = UpdateCommandDesc->newWorkDim;
  if (!NewWorkDim && Command->Kernel != UpdateCommandDesc->hNewKernel) {
    return UR_RESULT_ERROR_INVALID_OPERATION;
  }

  if (NewWorkDim) {
    UR_ASSERT(NewWorkDim > 0, UR_RESULT_ERROR_INVALID_WORK_DIMENSION);
    UR_ASSERT(NewWorkDim < 4, UR_RESULT_ERROR_INVALID_WORK_DIMENSION);

    if (NewWorkDim != Command->WorkDim &&
        Command->Kernel == UpdateCommandDesc->hNewKernel) {
      return UR_RESULT_ERROR_INVALID_OPERATION;
    }

    // Error If Local size and not global size
    if ((UpdateCommandDesc->pNewLocalWorkSize != nullptr) &&
        (UpdateCommandDesc->pNewGlobalWorkSize == nullptr)) {
      return UR_RESULT_ERROR_INVALID_OPERATION;
    }

    // Error if local size non-nullptr and created with null
    // or if local size nullptr and created with non-null
    const bool IsNewLocalSizeNull =
        UpdateCommandDesc->pNewLocalWorkSize == nullptr;
    const bool IsOriginalLocalSizeNull = Command->isNullLocalSize();

    if (IsNewLocalSizeNull ^ IsOriginalLocalSizeNull) {
      return UR_RESULT_ERROR_INVALID_OPERATION;
    }
  }

  if (!Command->ValidKernelHandles.count(UpdateCommandDesc->hNewKernel)) {
    return UR_RESULT_ERROR_INVALID_VALUE;
  }
  return UR_RESULT_SUCCESS;
}

/**
 * Updates the arguments of CommandDesc->hNewKernel
 * @param[in] Device The device associated with the kernel being updated.
 * @param[in] UpdateCommandDesc The update command description that contains the
 * new kernel and its arguments.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t
updateKernelArguments(ur_device_handle_t Device,
                      const ur_exp_command_buffer_update_kernel_launch_desc_t
                          *UpdateCommandDesc) {

  ur_kernel_handle_t NewKernel = UpdateCommandDesc->hNewKernel;

  // Update pointer arguments to the kernel
  uint32_t NumPointerArgs = UpdateCommandDesc->numNewPointerArgs;
  const ur_exp_command_buffer_update_pointer_arg_desc_t *ArgPointerList =
      UpdateCommandDesc->pNewPointerArgList;
  for (uint32_t i = 0; i < NumPointerArgs; i++) {
    const auto &PointerArgDesc = ArgPointerList[i];
    uint32_t ArgIndex = PointerArgDesc.argIndex;
    const void *ArgValue = PointerArgDesc.pNewPointerArg;

    ur_result_t Result = UR_RESULT_SUCCESS;
    try {
      NewKernel->setKernelArg(ArgIndex, sizeof(ArgValue), ArgValue);
    } catch (ur_result_t Err) {
      Result = Err;
      return Result;
    }
  }

  // Update memobj arguments to the kernel
  uint32_t NumMemobjArgs = UpdateCommandDesc->numNewMemObjArgs;
  const ur_exp_command_buffer_update_memobj_arg_desc_t *ArgMemobjList =
      UpdateCommandDesc->pNewMemObjArgList;
  for (uint32_t i = 0; i < NumMemobjArgs; i++) {
    const auto &MemobjArgDesc = ArgMemobjList[i];
    uint32_t ArgIndex = MemobjArgDesc.argIndex;
    ur_mem_handle_t ArgValue = MemobjArgDesc.hNewMemObjArg;

    ur_result_t Result = UR_RESULT_SUCCESS;
    try {
      if (ArgValue == nullptr) {
        NewKernel->setKernelArg(ArgIndex, 0, nullptr);
      } else {
        CUdeviceptr CuPtr = std::get<BufferMem>(ArgValue->Mem).getPtr(Device);
        NewKernel->setKernelArg(ArgIndex, sizeof(CUdeviceptr), (void *)&CuPtr);
      }
    } catch (ur_result_t Err) {
      Result = Err;
      return Result;
    }
  }

  // Update value arguments to the kernel
  uint32_t NumValueArgs = UpdateCommandDesc->numNewValueArgs;
  const ur_exp_command_buffer_update_value_arg_desc_t *ArgValueList =
      UpdateCommandDesc->pNewValueArgList;
  for (uint32_t i = 0; i < NumValueArgs; i++) {
    const auto &ValueArgDesc = ArgValueList[i];
    uint32_t ArgIndex = ValueArgDesc.argIndex;
    size_t ArgSize = ValueArgDesc.argSize;
    const void *ArgValue = ValueArgDesc.pNewValueArg;

    ur_result_t Result = UR_RESULT_SUCCESS;
    try {
      NewKernel->setKernelArg(ArgIndex, ArgSize, ArgValue);
    } catch (ur_result_t Err) {
      Result = Err;
      return Result;
    }
  }

  return UR_RESULT_SUCCESS;
}

/**
 * Updates the command buffer command with new values from the update
 * description.
 * @param[in] Command The command to be updated.
 * @param[in] UpdateCommandDesc The update command description.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t
updateCommand(ur_exp_command_buffer_command_handle_t Command,
              const ur_exp_command_buffer_update_kernel_launch_desc_t
                  *UpdateCommandDesc) {

  Command->Kernel = UpdateCommandDesc->hNewKernel;

  if (UpdateCommandDesc->newWorkDim) {
    Command->WorkDim = UpdateCommandDesc->newWorkDim;
  }

  if (UpdateCommandDesc->pNewGlobalWorkOffset) {
    Command->setGlobalOffset(UpdateCommandDesc->pNewGlobalWorkOffset);
  }

  if (UpdateCommandDesc->pNewGlobalWorkSize) {
    Command->setGlobalSize(UpdateCommandDesc->pNewGlobalWorkSize);
  }

  if (UpdateCommandDesc->pNewLocalWorkSize) {
    Command->setLocalSize(UpdateCommandDesc->pNewLocalWorkSize);
  }

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferUpdateKernelLaunchExp(
    ur_exp_command_buffer_command_handle_t hCommand,
    const ur_exp_command_buffer_update_kernel_launch_desc_t
        *pUpdateKernelLaunch) {

  ur_exp_command_buffer_handle_t CommandBuffer = hCommand->CommandBuffer;

  UR_CHECK_ERROR(validateCommandDesc(hCommand, pUpdateKernelLaunch));
  UR_CHECK_ERROR(
      updateKernelArguments(CommandBuffer->Device, pUpdateKernelLaunch));
  UR_CHECK_ERROR(updateCommand(hCommand, pUpdateKernelLaunch));

  // If no work-size is provided make sure we pass nullptr to setKernelParams so
  // it can guess the local work size.
  const bool ProvidedLocalSize = !hCommand->isNullLocalSize();
  size_t *LocalWorkSize = ProvidedLocalSize ? hCommand->LocalWorkSize : nullptr;

  // Set the number of threads per block to the number of threads per warp
  // by default unless user has provided a better number.
  size_t ThreadsPerBlock[3] = {32u, 1u, 1u};
  size_t BlocksPerGrid[3] = {1u, 1u, 1u};
  CUfunction CuFunc = hCommand->Kernel->get();
  auto Result = setKernelParams(
      CommandBuffer->Context, CommandBuffer->Device, hCommand->WorkDim,
      hCommand->GlobalWorkOffset, hCommand->GlobalWorkSize, LocalWorkSize,
      hCommand->Kernel, CuFunc, ThreadsPerBlock, BlocksPerGrid);
  if (Result != UR_RESULT_SUCCESS) {
    return Result;
  }

  CUDA_KERNEL_NODE_PARAMS &Params = hCommand->Params;

  Params.func = CuFunc;
  Params.gridDimX = BlocksPerGrid[0];
  Params.gridDimY = BlocksPerGrid[1];
  Params.gridDimZ = BlocksPerGrid[2];
  Params.blockDimX = ThreadsPerBlock[0];
  Params.blockDimY = ThreadsPerBlock[1];
  Params.blockDimZ = ThreadsPerBlock[2];
  Params.sharedMemBytes = hCommand->Kernel->getLocalSize();
  Params.kernelParams =
      const_cast<void **>(hCommand->Kernel->getArgIndices().data());

  CUgraphNode Node = hCommand->Node;
  CUgraphExec CudaGraphExec = CommandBuffer->CudaGraphExec;
  UR_CHECK_ERROR(cuGraphExecKernelNodeSetParams(CudaGraphExec, Node, &Params));
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferGetInfoExp(
    ur_exp_command_buffer_handle_t hCommandBuffer,
    ur_exp_command_buffer_info_t propName, size_t propSize, void *pPropValue,
    size_t *pPropSizeRet) {
  UrReturnHelper ReturnValue(propSize, pPropValue, pPropSizeRet);

  switch (propName) {
  case UR_EXP_COMMAND_BUFFER_INFO_REFERENCE_COUNT:
    return ReturnValue(hCommandBuffer->getExternalReferenceCount());
  default:
    assert(!"Command-buffer info request not implemented");
  }

  return UR_RESULT_ERROR_INVALID_ENUMERATION;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferCommandGetInfoExp(
    ur_exp_command_buffer_command_handle_t hCommand,
    ur_exp_command_buffer_command_info_t propName, size_t propSize,
    void *pPropValue, size_t *pPropSizeRet) {
  UrReturnHelper ReturnValue(propSize, pPropValue, pPropSizeRet);

  switch (propName) {
  case UR_EXP_COMMAND_BUFFER_COMMAND_INFO_REFERENCE_COUNT:
    return ReturnValue(hCommand->getExternalReferenceCount());
  default:
    assert(!"Command-buffer command info request not implemented");
  }

  return UR_RESULT_ERROR_INVALID_ENUMERATION;
}
