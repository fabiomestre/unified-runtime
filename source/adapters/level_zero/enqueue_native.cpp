//===--------- enqueue_native.cpp - LevelZero Adapter ---------------------===//
//
// Copyright (C) 2024 Intel Corporation
//
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <ur_api.h>

#include "logger/ur_logger.hpp"
#include "queue.hpp"
#include "ur_level_zero.hpp"

ur_result_t ur_queue_handle_legacy_t_::enqueueNativeCommandExp(
    ur_exp_enqueue_native_command_function_t pfnNativeEnqueue, void *data,
    uint32_t, const ur_mem_handle_t *,
    const ur_exp_enqueue_native_command_properties_t *,
    uint32_t NumEventsInWaitList, const ur_event_handle_t *phEventList,
    ur_event_handle_t *phEvent) {
  auto Queue = this;

  // Lock automatically releases when this goes out of scope.
  std::scoped_lock<ur_shared_mutex> lock(Queue->Mutex);

  bool UseCopyEngine = false;

  // Please note that the following code should be run before the
  // subsequent getAvailableCommandList() call so that there is no
  // dead-lock from waiting unsubmitted events in an open batch.
  // The createAndRetainUrZeEventList() has the proper side-effect
  // of submitting batches with dependent events.
  //
  _ur_ze_event_list_t TmpWaitList;
  UR_CALL(TmpWaitList.createAndRetainUrZeEventList(
      NumEventsInWaitList, phEventList, Queue, UseCopyEngine));

  // Get a new command list to be used on this call
  ur_command_list_ptr_t CommandList{};
  // TODO: Change UseCopyEngine argument to 'true' once L0 backend
  // support is added
  UR_CALL(Queue->Context->getAvailableCommandList(
      Queue, CommandList, UseCopyEngine, NumEventsInWaitList, phEventList));

  // TODO: do we need to create a unique command type for this?
  ze_event_handle_t ZeEvent = nullptr;
  ur_event_handle_t InternalEvent;
  bool IsInternal = phEvent == nullptr;
  ur_event_handle_t *Event = phEvent ? phEvent : &InternalEvent;
  UR_CALL(createEventAndAssociateQueue(Queue, Event, UR_COMMAND_USM_PREFETCH,
                                       CommandList, IsInternal, false));
  ZeEvent = (*Event)->ZeEvent;
  (*Event)->WaitList = TmpWaitList;

  const auto &WaitList = (*Event)->WaitList;
  const auto &ZeCommandList = CommandList->first;
  if (WaitList.Length) {
    ZE2UR_CALL(zeCommandListAppendWaitOnEvents,
               (ZeCommandList, WaitList.Length, WaitList.ZeEventList));
  }
  // Execute interop func
  pfnNativeEnqueue(Queue, data);

  // TODO: Level Zero does not have a completion "event" with the prefetch API,
  // so manually add command to signal our event.
  ZE2UR_CALL(zeCommandListAppendSignalEvent, (ZeCommandList, ZeEvent));

  UR_CALL(Queue->executeCommandList(CommandList, false));

  return UR_RESULT_SUCCESS;
}
