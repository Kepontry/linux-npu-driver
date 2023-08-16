/*
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "umd_common.hpp"

#include "vpu_driver/source/device/vpu_device_context.hpp"
#include "vpu_driver/source/command/vpu_command_buffer.hpp"
#include "vpu_driver/source/command/vpu_copy_command.hpp"
#include "vpu_driver/source/utilities/log.hpp"

namespace VPU {

VPUCommandBuffer::VPUCommandBuffer(VPUDeviceContext *ctx, VPUBufferObject *buffer, Target target)
    : ctx(ctx)
    , buffer(buffer)
    , targetEngine(target)
    , jobStatus(std::numeric_limits<uint32_t>::max()) {
    bufferHandles.emplace_back(buffer->getHandle());
}

VPUCommandBuffer::~VPUCommandBuffer() {
    if (ctx && buffer)
        ctx->freeMemAlloc(buffer);
}

std::unique_ptr<VPUCommandBuffer>
VPUCommandBuffer::allocateCommandBuffer(VPUDeviceContext *ctx,
                                        const std::vector<std::shared_ptr<VPUCommand>> &cmds,
                                        VPUCommandBuffer::Target engineType,
                                        VPUEventCommand::KMDEventDataType **fenceWait) {
    if (ctx == nullptr || cmds.empty()) {
        LOG_E("VPUDeviceContext is nullptr or command list is empty");
        return nullptr;
    }

    size_t cmdSize = 0;
    size_t descriptorSize = 0;
    for (const auto &cmd : cmds) {
        cmdSize += cmd->getCommitSize();
        descriptorSize += getFwDataCacheAlign(cmd->getDescriptorSize());
    }

    if (fenceWait) {
        if (*fenceWait) {
            // Add internal fence wait command
            cmdSize += sizeof(vpu_cmd_fence_t);
        }

        // Add internal fence signal command
        cmdSize += sizeof(vpu_cmd_fence_t);
    }

    VPUBufferObject *buffer = ctx->createInternalBufferObject(
        sizeof(CommandHeader) + getFwDataCacheAlign(cmdSize) + descriptorSize,
        VPUBufferObject::Type::CachedLow);
    if (buffer == nullptr) {
        LOG_E("Failed to allocate buffer object for command buffer for %s engine",
              targetEngineToStr(engineType));
        return nullptr;
    }

    auto cmdBuffer = std::make_unique<VPUCommandBuffer>(ctx, buffer, engineType);
    if (!cmdBuffer->initHeader(cmdSize)) {
        LOG_E("Failed to initialize VPUCommandBuffer - %s", cmdBuffer->getName());
        return nullptr;
    }

    uint64_t cmdOffset = offsetof(CommandHeader, commandList);
    uint64_t descOffset = cmdOffset + getFwDataCacheAlign(cmdSize);

    if (fenceWait && *fenceWait) {
        auto waitCmd = VPUEventWaitCommand::create(ctx, *fenceWait);
        if (waitCmd == nullptr) {
            LOG_E("Failed to initialize wait event command");
            return nullptr;
        }

        if (!cmdBuffer->addCommand(waitCmd.get(), cmdOffset, descOffset)) {
            LOG_E("Failed to append wait command to buffer");
            return nullptr;
        }
    }

    for (auto &cmd : cmds) {
        if (cmd->isSynchronizeCommand() && !cmdBuffer->setSyncFenceAddr(cmd.get())) {
            LOG_E("Failed to set synchronize fence vpu addresss");
            return nullptr;
        }

        if (cmd->isCopyTypeCommand()) {
            if (!cmd->changeCopyCommandType(static_cast<uint32_t>(engineType))) {
                LOG_E("Engine mismatch, can not append copy command to buffer");
                return nullptr;
            }
        }

        if (!cmdBuffer->addCommand(cmd.get(), cmdOffset, descOffset)) {
            LOG_E("Failed to append command to buffer");
            return nullptr;
        }
    }

    if (fenceWait) {
        VPUEventCommand::KMDEventDataType *fenceSignal = reinterpret_cast<uint64_t *>(
            buffer->getBasePointer() + offsetof(CommandHeader, fenceValue));
        auto signalCmd = VPUEventSignalCommand::create(ctx, fenceSignal);
        if (signalCmd == nullptr) {
            LOG_E("Failed to create signal event Command");
            return nullptr;
        }

        if (!cmdBuffer->addCommand(signalCmd.get(), cmdOffset, descOffset)) {
            LOG_E("Failed to append signal command to buffer");
            return nullptr;
        }

        *fenceWait = fenceSignal;
    }

    if (cmdOffset != offsetof(CommandHeader, commandList) + cmdSize) {
        LOG_E("Invalid size of command list");
        return nullptr;
    }

    if (descOffset !=
        offsetof(CommandHeader, commandList) + getFwDataCacheAlign(cmdSize) + descriptorSize) {
        LOG_E("Invalid size of descriptor");
        return nullptr;
    }

    cmdBuffer->printCommandBuffer();
    return cmdBuffer;
}

bool VPUCommandBuffer::initHeader(size_t cmdSize) {
    if (buffer == nullptr) {
        LOG_E("Invalid command buffer pointer is passed");
        return false;
    }

    vpu_cmd_buffer_header_t *bb =
        reinterpret_cast<vpu_cmd_buffer_header_t *>(buffer->getBasePointer());

    bb->cmd_offset = offsetof(CommandHeader, commandList);
    bb->cmd_buffer_size = boost::numeric_cast<uint32_t>(bb->cmd_offset + cmdSize);
    bb->context_save_area_address = buffer->getVPUAddr() + offsetof(CommandHeader, contextSaveArea);

    uint64_t baseAddress = ctx->getVPULowBaseAddress();
    if (baseAddress == 0) {
        LOG_E("Invalid base address for VPU");
        return false;
    }

    bb->kernel_heap_base_address = baseAddress;
    bb->descriptor_heap_base_address = baseAddress;
    bb->fence_heap_base_address = baseAddress;

    return true;
}

bool VPUCommandBuffer::addCommand(VPUCommand *cmd, uint64_t &cmdOffset, uint64_t &descOffset) {
    if (cmd == nullptr) {
        LOG_E("Command is nullptr or command is not initialized");
        return false;
    }

    LOG_I("Attempting append a command %#x (size: %zu) to %s buffer",
          cmd->getCommandType(),
          cmd->getCommitSize(),
          getName());

    if (cmdOffset >= descOffset) {
        LOG_E("Command override the descriptor");
        return false;
    }

    for (const auto &bo : cmd->getAssociateBufferObjects()) {
        auto it = std::find(bufferHandles.begin(), bufferHandles.end(), bo->getHandle());
        if (it == bufferHandles.end())
            bufferHandles.emplace_back(bo->getHandle());
    }

    void *descPtr = buffer->getBasePointer() + descOffset;
    if (!cmd->copyDescriptor(ctx, &descPtr)) {
        LOG_E("Failed to update offset in command");
        return false;
    }

    if (!buffer->copyToBuffer(cmd->getCommitStream(), cmd->getCommitSize(), cmdOffset)) {
        LOG_E("Failed to copy command structure to command buffer");
        return false;
    }

    LOG_V("Command appended to %s buffer: cmdOffset: %zu",
          targetEngineToStr(targetEngine),
          cmdOffset);

    cmdOffset += cmd->getCommitSize();
    descOffset += getFwDataCacheAlign(cmd->getDescriptorSize());
    return true;
}

bool VPUCommandBuffer::setSyncFenceAddr(VPUCommand *cmd) {
    if (syncFenceVpuAddr != 0) {
        LOG_E("Synchronize Fence VPU Address is already set");
        return false;
    }

    if (cmd->getCommandType() != VPU_CMD_FENCE_SIGNAL) {
        LOG_E("Not supported command type for synchronize command");
        return false;
    }

    auto *fenceSignalHeader = reinterpret_cast<const vpu_cmd_fence_t *>(cmd->getCommitStream());
    syncFenceVpuAddr = ctx->getVPULowBaseAddress() + fenceSignalHeader->offset;
    return true;
}

bool VPUCommandBuffer::waitForCompletion(int64_t timeout_abs_ns) {
    drm_ivpu_bo_wait args = {};
    args.handle = buffer->getHandle();
    args.timeout_ns = timeout_abs_ns;
    args.job_status = std::numeric_limits<uint32_t>::max();

    int ret = ctx->getDriverApi().wait(&args);
    LOG_I("Wait completed: ret = %d, errno = %d, commandBuffer: %p", ret, errno, this);
    if (ret != 0)
        return false;

    jobStatus = args.job_status;
    return true;
}

void VPUCommandBuffer::printDesc(vpu_cmd_resource_descriptor_table_t *table,
                                 size_t size,
                                 const char *type) const {
    if (table == nullptr) {
        LOG_I("Descriptor base pointer is not provided, skip printing descriptors");
        return;
    }

    LOG_I("Blob %s command descriptor table (table pointer: %p, size: %lu):", type, table, size);

    for (int i = 0; size > sizeof(vpu_cmd_resource_descriptor); i++) {
        LOG_I("Table: %i:\n\ttype = %#x\n\tdesc_count = %i", i, table->type, table->desc_count);
        size_t count = table->desc_count;
        table++;

        vpu_cmd_resource_descriptor *desc = reinterpret_cast<decltype(desc)>(table);
        for (size_t j = 0; j < count; j++) {
            LOG_I("Entry %lu:\n\taddress = %#lx\n\twidth = %#x", j, desc->address, desc->width);
            desc++;
        }

        table = reinterpret_cast<vpu_cmd_resource_descriptor_table_t *>(desc);
        size -= count * sizeof(vpu_cmd_resource_descriptor) +
                sizeof(vpu_cmd_resource_descriptor_table_t);
    }
}

void VPUCommandBuffer::printCommandBuffer() const {
    if (getLogLevel() < LogLevel::INFO)
        return;

    uint8_t *bufferPtr = buffer->getBasePointer();
    vpu_cmd_buffer_header_t *cmdHeader = reinterpret_cast<vpu_cmd_buffer_header_t *>(bufferPtr);
    LOG_I("Start %s command buffer printing:\n"
          "\tCommand buffer ptr cpu = %p, vpu = %#lx\n"
          "\tSize = %u bytes, commands offset %u\n"
          "\tKernel heap addr = %#lx\n"
          "\tDescriptor heap addr = %#lx\n"
          "\tFence heap addr = %#lx",
          targetEngineToStr(targetEngine),
          cmdHeader,
          buffer->getVPUAddr(),
          cmdHeader->cmd_buffer_size,
          cmdHeader->cmd_offset,
          cmdHeader->kernel_heap_base_address,
          cmdHeader->descriptor_heap_base_address,
          cmdHeader->fence_heap_base_address);

    size_t cmdOffset = cmdHeader->cmd_offset;
    int i = 0;
    while (cmdOffset < cmdHeader->cmd_buffer_size) {
        vpu_cmd_header_t *cmd = reinterpret_cast<vpu_cmd_header_t *>(bufferPtr + cmdOffset);

        if (cmd->size <= 0 || cmd->size >= cmdHeader->cmd_buffer_size) {
            LOG_E("Invalid command size, stop command buffer printing");
            return;
        }

        switch (cmd->type) {
        case VPU_CMD_TIMESTAMP:
            LOG_I("Command %i: Timestamp (size: %u bytes)\n"
                  "\ttimestamp_address = %#lx",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_timestamp_t *>(cmd)->timestamp_address);
            break;
        case VPU_CMD_FENCE_WAIT:
            LOG_I("Command %i: Fence Wait (size: %u bytes)\n"
                  "\toffset = %#lx, value = %#lx",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_fence_t *>(cmd)->offset,
                  reinterpret_cast<vpu_cmd_fence_t *>(cmd)->value);
            break;
        case VPU_CMD_FENCE_SIGNAL:
            LOG_I("Command %i: Fence Signal (size: %u bytes)\n"
                  "\toffset = %#lx, value = %#lx",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_fence_t *>(cmd)->offset,
                  reinterpret_cast<vpu_cmd_fence_t *>(cmd)->value);
            break;
        case VPU_CMD_BARRIER:
            LOG_I("Command %i: Barrier (size: %u bytes)", i, cmd->size);
            break;
        case VPU_CMD_METRIC_QUERY_BEGIN:
            LOG_I("Command %i: Metric Query Begin (size: %u bytes)\n"
                  "\tmetric_group_type = %u, metric_data_address = %#lx",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_metric_query_t *>(cmd)->metric_group_type,
                  reinterpret_cast<vpu_cmd_metric_query_t *>(cmd)->metric_data_address);
            break;
        case VPU_CMD_METRIC_QUERY_END:
            LOG_I("Command %i: Metric Query End (size: %u bytes)\n"
                  "\tmetric_group_type = %u, metric_data_address = %#lx",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_metric_query_t *>(cmd)->metric_group_type,
                  reinterpret_cast<vpu_cmd_metric_query_t *>(cmd)->metric_data_address);
            break;
        case VPU_CMD_COPY_SYSTEM_TO_SYSTEM:
            LOG_I("Command %i: Copy System to System (size: %u bytes)\n"
                  "\tdesc_start_offset = %#lx, desc_count = %u",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_copy_buffer_t *>(cmd)->desc_start_offset,
                  reinterpret_cast<vpu_cmd_copy_buffer_t *>(cmd)->desc_count);
            ctx->printCopyDescriptor(
                bufferPtr + reinterpret_cast<vpu_cmd_copy_buffer_t *>(cmd)->desc_start_offset +
                    cmdHeader->descriptor_heap_base_address - buffer->getVPUAddr(),
                cmd);
            break;
        case VPU_CMD_COPY_LOCAL_TO_LOCAL:
            LOG_I("Command %i: Copy Local to Local (size: %u bytes)\n"
                  "\tdesc_start_offset = %#lx, desc_count = %u",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_copy_buffer_t *>(cmd)->desc_start_offset,
                  reinterpret_cast<vpu_cmd_copy_buffer_t *>(cmd)->desc_count);
            ctx->printCopyDescriptor(
                bufferPtr + reinterpret_cast<vpu_cmd_copy_buffer_t *>(cmd)->desc_start_offset +
                    cmdHeader->descriptor_heap_base_address - buffer->getVPUAddr(),
                cmd);
            break;
        case VPU_CMD_OV_BLOB_INITIALIZE:
            LOG_I("Command %i: OV Blob Initialize (size: %u bytes)\n"
                  "\tkernel_size = %u, kernel_offset = %#lx, desc_table_size = %u, "
                  " desc_table_offset = %#lx, blob_id = %lu",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_ov_blob_initialize_t *>(cmd)->kernel_size,
                  reinterpret_cast<vpu_cmd_ov_blob_initialize_t *>(cmd)->kernel_offset,
                  reinterpret_cast<vpu_cmd_ov_blob_initialize_t *>(cmd)->desc_table_size,
                  reinterpret_cast<vpu_cmd_ov_blob_initialize_t *>(cmd)->desc_table_offset,
                  reinterpret_cast<vpu_cmd_ov_blob_initialize_t *>(cmd)->blob_id);
            printDesc(reinterpret_cast<vpu_cmd_resource_descriptor_table_t *>(
                          bufferPtr +
                          reinterpret_cast<vpu_cmd_ov_blob_initialize_t *>(cmd)->desc_table_offset +
                          cmdHeader->descriptor_heap_base_address - buffer->getVPUAddr()),
                      reinterpret_cast<vpu_cmd_ov_blob_initialize_t *>(cmd)->desc_table_size,
                      "Initialize");
            break;
        case VPU_CMD_OV_BLOB_EXECUTE:
            LOG_I("Command %i: OV Blob Execute (size: %u bytes)\n"
                  "\tdesc_table_size = %u, desc_table_offset = %#lx, blob_id = %lu",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_ov_blob_execute_t *>(cmd)->desc_table_size,
                  reinterpret_cast<vpu_cmd_ov_blob_execute_t *>(cmd)->desc_table_offset,
                  reinterpret_cast<vpu_cmd_ov_blob_execute_t *>(cmd)->blob_id);
            printDesc(reinterpret_cast<vpu_cmd_resource_descriptor_table_t *>(
                          bufferPtr +
                          reinterpret_cast<vpu_cmd_ov_blob_execute_t *>(cmd)->desc_table_offset +
                          cmdHeader->descriptor_heap_base_address - buffer->getVPUAddr()),
                      reinterpret_cast<vpu_cmd_ov_blob_execute_t *>(cmd)->desc_table_size,
                      "Execute");
            break;
        case VPU_CMD_INFERENCE_EXECUTE:
            LOG_I(
                "Command %i: Inference Execute (size %u bytes)\n"
                "\tinference_id = %lu, host_mapped_inference.address = %#lx, "
                "host_mapped_inference.size = %#x",
                i,
                cmd->size,
                reinterpret_cast<vpu_cmd_inference_execute_t *>(cmd)->inference_id,
                reinterpret_cast<vpu_cmd_inference_execute_t *>(cmd)->host_mapped_inference.address,
                reinterpret_cast<vpu_cmd_inference_execute_t *>(cmd)->host_mapped_inference.width);
            break;
        case VPU_CMD_MEMORY_FILL:
            LOG_I("Command %i: Memory fill (size: %u bytes) pattern = 0x%x addr=0x%lx",
                  i,
                  cmd->size,
                  reinterpret_cast<vpu_cmd_memory_fill_t *>(cmd)->fill_pattern,
                  reinterpret_cast<vpu_cmd_memory_fill_t *>(cmd)->start_address);
            break;

        default:
            LOG_E("Unknown command, stop command buffer printing");
            return;
        }
        cmdOffset += cmd->size;
        i++;
    }
    LOG_I("Stop %s command buffer printing", targetEngineToStr(targetEngine));
}

} // namespace VPU
