/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_queue.h"

#include "venus-protocol/vn_protocol_renderer_queue.h"

#include "vkr_context.h"
#include "vkr_physical_device.h"
#include "vkr_queue_gen.h"

static struct vkr_queue_sync *
vkr_device_alloc_queue_sync(struct vkr_device *dev,
                            uint32_t fence_flags,
                            uint32_t ring_idx,
                            uint64_t fence_id)
{
   struct vn_device_proc_table *vk = &dev->proc_table;
   struct vkr_queue_sync *sync;

   mtx_lock(&dev->free_sync_mutex);
   if (LIST_IS_EMPTY(&dev->free_syncs)) {
      mtx_unlock(&dev->free_sync_mutex);

      sync = malloc(sizeof(*sync));
      if (!sync)
         return NULL;

      const VkExportFenceCreateInfo export_info = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
         .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      const struct VkFenceCreateInfo create_info = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
         .pNext = dev->physical_device->KHR_external_fence_fd ? &export_info : NULL,
      };
      VkResult result =
         vk->CreateFence(dev->base.handle.device, &create_info, NULL, &sync->fence);
      if (result != VK_SUCCESS) {
         free(sync);
         vkr_log("failed to create sync fence for fence_id %" PRIu64, fence_id);
         return NULL;
      }
   } else {
      sync = LIST_ENTRY(struct vkr_queue_sync, dev->free_syncs.next, head);
      list_del(&sync->head);
      mtx_unlock(&dev->free_sync_mutex);

      vk->ResetFences(dev->base.handle.device, 1, &sync->fence);
   }

   sync->device_lost = false;
   sync->flags = fence_flags;
   sync->ring_idx = ring_idx;
   sync->fence_id = fence_id;

   return sync;
}

static void
vkr_device_free_queue_sync(struct vkr_device *dev, struct vkr_queue_sync *sync)
{
   mtx_lock(&dev->free_sync_mutex);
   list_addtail(&sync->head, &dev->free_syncs);
   mtx_unlock(&dev->free_sync_mutex);
}

static inline void
vkr_queue_sync_retire(struct vkr_queue *queue, struct vkr_queue_sync *sync)
{
   queue->context->retire_fence(queue->context->ctx_id, sync->ring_idx, sync->fence_id);
   vkr_device_free_queue_sync(queue->device, sync);
}

bool
vkr_queue_sync_submit(struct vkr_queue *queue,
                      uint32_t flags,
                      uint32_t ring_idx,
                      uint64_t fence_id)
{
   struct vkr_device *dev = queue->device;
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_queue_sync *sync =
      vkr_device_alloc_queue_sync(dev, flags, ring_idx, fence_id);
   if (!sync)
      return false;

   VkResult result = vk->QueueSubmit(queue->base.handle.queue, 0, NULL, sync->fence);
   if (result == VK_ERROR_DEVICE_LOST) {
      sync->device_lost = true;
      vkr_log("sync submit hit device lost for fence_id %" PRIu64, fence_id);
   } else if (result != VK_SUCCESS) {
      vkr_device_free_queue_sync(dev, sync);
      vkr_log("sync submit failed (vk ret %d) for fence_id %" PRIu64, result, fence_id);
      return false;
   }

   mtx_lock(&queue->mutex);
   list_addtail(&sync->head, &queue->pending_syncs);
   mtx_unlock(&queue->mutex);
   cnd_signal(&queue->cond);

   return true;
}

static void
vkr_queue_retire_all_syncs(struct vkr_queue *queue)
{
   mtx_lock(&queue->mutex);
   queue->join = true;
   mtx_unlock(&queue->mutex);

   cnd_signal(&queue->cond);
   thrd_join(queue->thread, NULL);

   struct vkr_queue_sync *sync, *tmp;
   LIST_FOR_EACH_ENTRY_SAFE (sync, tmp, &queue->pending_syncs, head)
      vkr_queue_sync_retire(queue, sync);
}

void
vkr_queue_destroy(struct vkr_context *ctx, struct vkr_queue *queue)
{
   /* vkDeviceWaitIdle has been called */
   vkr_queue_retire_all_syncs(queue);

   mtx_destroy(&queue->mutex);
   cnd_destroy(&queue->cond);

   list_del(&queue->base.track_head);

   if (queue->ring_idx > 0)
      ctx->sync_queues[queue->ring_idx] = NULL;

   if (queue->base.id)
      vkr_context_remove_object(ctx, &queue->base);
   else
      free(queue);
}

static int
vkr_queue_thread(void *arg)
{
   struct vkr_queue *queue = arg;
   struct vkr_context *ctx = queue->context;
   struct vkr_device *dev = queue->device;
   struct vn_device_proc_table *vk = &dev->proc_table;
   const uint64_t ns_per_sec = 1000000000llu;
   char thread_name[16];

   snprintf(thread_name, ARRAY_SIZE(thread_name), "vkr-queue-%d", ctx->ctx_id);
   u_thread_setname(thread_name);

   mtx_lock(&queue->mutex);
   while (true) {
      while (LIST_IS_EMPTY(&queue->pending_syncs) && !queue->join)
         cnd_wait(&queue->cond, &queue->mutex);

      if (queue->join)
         break;

      struct vkr_queue_sync *sync =
         LIST_ENTRY(struct vkr_queue_sync, queue->pending_syncs.next, head);

      mtx_unlock(&queue->mutex);

      VkResult result;
      if (sync->device_lost) {
         result = VK_ERROR_DEVICE_LOST;
      } else {
         result = vk->WaitForFences(dev->base.handle.device, 1, &sync->fence, true,
                                    ns_per_sec * 3);
      }

      mtx_lock(&queue->mutex);

      if (result == VK_TIMEOUT)
         continue;

      list_del(&sync->head);

      vkr_queue_sync_retire(queue, sync);
   }
   mtx_unlock(&queue->mutex);

   return 0;
}

struct vkr_queue *
vkr_queue_create(struct vkr_context *ctx,
                 struct vkr_device *dev,
                 VkDeviceQueueCreateFlags flags,
                 uint32_t family,
                 uint32_t index,
                 VkQueue handle)
{
   struct vkr_queue *queue;
   int ret;

   /* id is set to 0 until vkr_queue_assign_object_id */
   queue = vkr_object_alloc(sizeof(*queue), VK_OBJECT_TYPE_QUEUE, 0);
   if (!queue)
      return NULL;

   queue->base.handle.queue = handle;

   queue->context = ctx;
   queue->device = dev;
   queue->flags = flags;
   queue->family = family;
   queue->index = index;

   list_inithead(&queue->pending_syncs);

   ret = mtx_init(&queue->mutex, mtx_plain);
   if (ret != thrd_success) {
      free(queue);
      return NULL;
   }
   ret = cnd_init(&queue->cond);
   if (ret != thrd_success) {
      mtx_destroy(&queue->mutex);
      free(queue);
      return NULL;
   }

   ret = thrd_create(&queue->thread, vkr_queue_thread, queue);
   if (ret != thrd_success) {
      mtx_destroy(&queue->mutex);
      cnd_destroy(&queue->cond);
      free(queue);
      return NULL;
   }

   list_inithead(&queue->base.track_head);

   return queue;
}

static void
vkr_queue_assign_object_id(struct vkr_context *ctx,
                           struct vkr_queue *queue,
                           vkr_object_id id)
{
   if (queue->base.id) {
      if (queue->base.id != id)
         vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }
   if (!vkr_context_validate_object_id(ctx, id))
      return;

   queue->base.id = id;

   vkr_context_add_object(ctx, &queue->base);
}

static struct vkr_queue *
vkr_device_lookup_queue(struct vkr_device *dev,
                        VkDeviceQueueCreateFlags flags,
                        uint32_t family,
                        uint32_t index)
{
   struct vkr_queue *queue;

   LIST_FOR_EACH_ENTRY (queue, &dev->queues, base.track_head) {
      if (queue->flags == flags && queue->family == family && queue->index == index)
         return queue;
   }

   return NULL;
}

static void
vkr_dispatch_vkGetDeviceQueue(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkGetDeviceQueue *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = vkr_device_from_handle(args->device);

   struct vkr_queue *queue = vkr_device_lookup_queue(
      dev, 0 /* flags */, args->queueFamilyIndex, args->queueIndex);
   if (!queue) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   const vkr_object_id id =
      vkr_cs_handle_load_id((const void **)args->pQueue, VK_OBJECT_TYPE_QUEUE);
   vkr_queue_assign_object_id(ctx, queue, id);
}

static void
vkr_dispatch_vkGetDeviceQueue2(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkGetDeviceQueue2 *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = vkr_device_from_handle(args->device);

   struct vkr_queue *queue = vkr_device_lookup_queue(dev, args->pQueueInfo->flags,
                                                     args->pQueueInfo->queueFamilyIndex,
                                                     args->pQueueInfo->queueIndex);
   if (!queue) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   const VkDeviceQueueTimelineInfoMESA *timeline_info = vkr_find_struct(
      args->pQueueInfo->pNext, VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA);
   if (timeline_info) {
      if (timeline_info->ringIdx == 0 ||
          timeline_info->ringIdx >= ARRAY_SIZE(ctx->sync_queues)) {
         vkr_log("invalid ring_idx %d", timeline_info->ringIdx);
         vkr_cs_decoder_set_fatal(&ctx->decoder);
         return;
      }

      if (ctx->sync_queues[timeline_info->ringIdx]) {
         vkr_log("sync_queue %d already bound", timeline_info->ringIdx);
         vkr_cs_decoder_set_fatal(&ctx->decoder);
         return;
      }

      queue->ring_idx = timeline_info->ringIdx;
      ctx->sync_queues[timeline_info->ringIdx] = queue;
   }

   const vkr_object_id id =
      vkr_cs_handle_load_id((const void **)args->pQueue, VK_OBJECT_TYPE_QUEUE);
   vkr_queue_assign_object_id(ctx, queue, id);
}

static void
vkr_dispatch_vkQueueSubmit(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkQueueSubmit *args)
{
   struct vkr_queue *queue = vkr_queue_from_handle(args->queue);
   struct vn_device_proc_table *vk = &queue->device->proc_table;

   vn_replace_vkQueueSubmit_args_handle(args);
   args->ret =
      vk->QueueSubmit(args->queue, args->submitCount, args->pSubmits, args->fence);
}

static void
vkr_dispatch_vkQueueBindSparse(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkQueueBindSparse *args)
{
   struct vkr_queue *queue = vkr_queue_from_handle(args->queue);
   struct vn_device_proc_table *vk = &queue->device->proc_table;

   vn_replace_vkQueueBindSparse_args_handle(args);
   args->ret =
      vk->QueueBindSparse(args->queue, args->bindInfoCount, args->pBindInfo, args->fence);
}

static void
vkr_dispatch_vkQueueWaitIdle(struct vn_dispatch_context *dispatch,
                             UNUSED struct vn_command_vkQueueWaitIdle *args)
{
   struct vkr_context *ctx = dispatch->data;
   /* no blocking call */
   vkr_cs_decoder_set_fatal(&ctx->decoder);
}

static void
vkr_dispatch_vkQueueSubmit2(UNUSED struct vn_dispatch_context *dispatch,
                            struct vn_command_vkQueueSubmit2 *args)
{
   struct vkr_queue *queue = vkr_queue_from_handle(args->queue);
   struct vn_device_proc_table *vk = &queue->device->proc_table;

   vn_replace_vkQueueSubmit2_args_handle(args);
   args->ret =
      vk->QueueSubmit2(args->queue, args->submitCount, args->pSubmits, args->fence);
}

static void
vkr_dispatch_vkCreateFence(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateFence *args)
{
   vkr_fence_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyFence(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyFence *args)
{
   vkr_fence_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkResetFences(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkResetFences *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkResetFences_args_handle(args);
   args->ret = vk->ResetFences(args->device, args->fenceCount, args->pFences);
}

static void
vkr_dispatch_vkGetFenceStatus(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkGetFenceStatus *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetFenceStatus_args_handle(args);
   args->ret = vk->GetFenceStatus(args->device, args->fence);
}

static void
vkr_dispatch_vkWaitForFences(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkWaitForFences *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkWaitForFences_args_handle(args);
   args->ret = vk->WaitForFences(args->device, args->fenceCount, args->pFences,
                                 args->waitAll, args->timeout);

   if (args->ret == VK_ERROR_DEVICE_LOST)
      vkr_cs_decoder_set_fatal(&ctx->decoder);
}

static void
vkr_dispatch_vkResetFenceResource100000MESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkResetFenceResource100000MESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   int fd = -1;

   vn_replace_vkResetFenceResource100000MESA_args_handle(args);

   const VkFenceGetFdInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
      .fence = args->fence,
      .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   VkResult result = vk->GetFenceFdKHR(args->device, &info, &fd);
   if (result != VK_SUCCESS) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   if (fd >= 0)
      close(fd);
}

static void
vkr_dispatch_vkCreateSemaphore(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateSemaphore *args)
{
   vkr_semaphore_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySemaphore(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroySemaphore *args)
{
   vkr_semaphore_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetSemaphoreCounterValue(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkGetSemaphoreCounterValue *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetSemaphoreCounterValue_args_handle(args);
   args->ret = vk->GetSemaphoreCounterValue(args->device, args->semaphore, args->pValue);
}

static void
vkr_dispatch_vkWaitSemaphores(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkWaitSemaphores *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkWaitSemaphores_args_handle(args);
   args->ret = vk->WaitSemaphores(args->device, args->pWaitInfo, args->timeout);

   if (args->ret == VK_ERROR_DEVICE_LOST)
      vkr_cs_decoder_set_fatal(&ctx->decoder);
}

static void
vkr_dispatch_vkSignalSemaphore(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkSignalSemaphore *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkSignalSemaphore_args_handle(args);
   args->ret = vk->SignalSemaphore(args->device, args->pSignalInfo);
}

static void
vkr_dispatch_vkWaitSemaphoreResource100000MESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkWaitSemaphoreResource100000MESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   int fd = -1;

   vn_replace_vkWaitSemaphoreResource100000MESA_args_handle(args);

   const VkSemaphoreGetFdInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .semaphore = args->semaphore,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   VkResult result = vk->GetSemaphoreFdKHR(args->device, &info, &fd);
   if (result != VK_SUCCESS) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   if (fd >= 0)
      close(fd);
}

static void
vkr_dispatch_vkImportSemaphoreResource100000MESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkImportSemaphoreResource100000MESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkImportSemaphoreResource100000MESA_args_handle(args);

   const VkImportSemaphoreResourceInfo100000MESA *res_info =
      args->pImportSemaphoreResourceInfo;

   /* resourceId 0 is for importing a signaled payload to sync_fd fence */
   assert(!res_info->resourceId);

   const VkImportSemaphoreFdInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
      .semaphore = res_info->semaphore,
      .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      .fd = -1,
   };
   if (vk->ImportSemaphoreFdKHR(args->device, &import_info) != VK_SUCCESS)
      vkr_cs_decoder_set_fatal(&ctx->decoder);
}

static void
vkr_dispatch_vkCreateEvent(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateEvent *args)
{
   vkr_event_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyEvent(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyEvent *args)
{
   vkr_event_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetEventStatus(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkGetEventStatus *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetEventStatus_args_handle(args);
   args->ret = vk->GetEventStatus(args->device, args->event);
}

static void
vkr_dispatch_vkSetEvent(UNUSED struct vn_dispatch_context *dispatch,
                        struct vn_command_vkSetEvent *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkSetEvent_args_handle(args);
   args->ret = vk->SetEvent(args->device, args->event);
}

static void
vkr_dispatch_vkResetEvent(UNUSED struct vn_dispatch_context *dispatch,
                          struct vn_command_vkResetEvent *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkResetEvent_args_handle(args);
   args->ret = vk->ResetEvent(args->device, args->event);
}

void
vkr_context_init_queue_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkGetDeviceQueue = vkr_dispatch_vkGetDeviceQueue;
   dispatch->dispatch_vkGetDeviceQueue2 = vkr_dispatch_vkGetDeviceQueue2;
   dispatch->dispatch_vkQueueSubmit = vkr_dispatch_vkQueueSubmit;
   dispatch->dispatch_vkQueueBindSparse = vkr_dispatch_vkQueueBindSparse;
   dispatch->dispatch_vkQueueWaitIdle = vkr_dispatch_vkQueueWaitIdle;

   /* VK_KHR_synchronization2 */
   dispatch->dispatch_vkQueueSubmit2 = vkr_dispatch_vkQueueSubmit2;
}

void
vkr_context_init_fence_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateFence = vkr_dispatch_vkCreateFence;
   dispatch->dispatch_vkDestroyFence = vkr_dispatch_vkDestroyFence;
   dispatch->dispatch_vkResetFences = vkr_dispatch_vkResetFences;
   dispatch->dispatch_vkGetFenceStatus = vkr_dispatch_vkGetFenceStatus;
   dispatch->dispatch_vkWaitForFences = vkr_dispatch_vkWaitForFences;

   dispatch->dispatch_vkResetFenceResource100000MESA =
      vkr_dispatch_vkResetFenceResource100000MESA;
}

void
vkr_context_init_semaphore_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSemaphore = vkr_dispatch_vkCreateSemaphore;
   dispatch->dispatch_vkDestroySemaphore = vkr_dispatch_vkDestroySemaphore;
   dispatch->dispatch_vkGetSemaphoreCounterValue =
      vkr_dispatch_vkGetSemaphoreCounterValue;
   dispatch->dispatch_vkWaitSemaphores = vkr_dispatch_vkWaitSemaphores;
   dispatch->dispatch_vkSignalSemaphore = vkr_dispatch_vkSignalSemaphore;

   dispatch->dispatch_vkWaitSemaphoreResource100000MESA =
      vkr_dispatch_vkWaitSemaphoreResource100000MESA;
   dispatch->dispatch_vkImportSemaphoreResource100000MESA =
      vkr_dispatch_vkImportSemaphoreResource100000MESA;
}

void
vkr_context_init_event_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateEvent = vkr_dispatch_vkCreateEvent;
   dispatch->dispatch_vkDestroyEvent = vkr_dispatch_vkDestroyEvent;
   dispatch->dispatch_vkGetEventStatus = vkr_dispatch_vkGetEventStatus;
   dispatch->dispatch_vkSetEvent = vkr_dispatch_vkSetEvent;
   dispatch->dispatch_vkResetEvent = vkr_dispatch_vkResetEvent;
}
