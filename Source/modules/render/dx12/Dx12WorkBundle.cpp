#include "Dx12WorkBundle.h"
#include "Dx12Device.h"
#include "Dx12Resources.h"
#include "Dx12ResourceCollection.h"
#include "Dx12ShaderDb.h"
#include "Dx12Utils.h"

namespace coalpy
{
namespace render
{

bool Dx12WorkBundle::load(const WorkBundle& workBundle)
{
    m_workBundle = workBundle;
    for (auto& it : workBundle.states)
        m_states[it.first] = getDx12GpuState(it.second.state);

    return true;
}

void Dx12WorkBundle::uploadAllTables()
{
    int totalDescriptors = m_workBundle.totalTableSize + m_workBundle.totalConstantBuffers;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srcDescBase;
    srcDescBase.reserve(totalDescriptors);

    std::vector<UINT> srcDescCounts;
    srcDescCounts.reserve(totalDescriptors);

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dstDescBase;
    dstDescBase.reserve(totalDescriptors);

    std::vector<UINT> dstDescCounts;
    dstDescCounts.reserve(totalDescriptors);

    Dx12ResourceCollection& resources = m_device.resources();
    if (m_srvUavTable.ownerHeap != nullptr && m_workBundle.totalTableSize)
    {
        for (auto& it : m_workBundle.tableAllocations)
        {
            CPY_ASSERT(it.first.valid());
            Dx12ResourceTable& t = resources.unsafeGetTable(it.first);
            const Dx12DescriptorTable& cpuTable = t.cpuTable();
            
            srcDescBase.push_back(cpuTable.baseHandle);
            srcDescCounts.push_back(cpuTable.count);

            dstDescBase.push_back(m_srvUavTable.getCpuHandle(it.second.offset));
            dstDescCounts.push_back(it.second.count);
            CPY_ASSERT(it.second.count == cpuTable.count);
        }
    }
    if (!dstDescBase.empty())
    {
        m_device.device().CopyDescriptors(
            (UINT)dstDescBase.size(),
            dstDescBase.data(), dstDescCounts.data(),
            (UINT)srcDescBase.size(),
            srcDescBase.data(), srcDescCounts.data(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

void Dx12WorkBundle::applyBarriers(const std::vector<ResourceBarrier>& barriers, ID3D12GraphicsCommandList6& outList)
{
    if (barriers.empty())
        return;

    std::vector<D3D12_RESOURCE_BARRIER> resultBarriers;
    resultBarriers.reserve(barriers.size());
    Dx12ResourceCollection& resources = m_device.resources();

    for (const auto& b : barriers)
    {
        if (b.prevState == b.postState)
            continue;

        Dx12Resource& r = resources.unsafeGetResource(b.resource);
        
        resultBarriers.emplace_back();
        D3D12_RESOURCE_BARRIER& d3d12barrier = resultBarriers.back();
        d3d12barrier = {};
        d3d12barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        if (b.type == BarrierType::Begin)
        {
            d3d12barrier.Flags |= D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
        }
        else if (b.type == BarrierType::End)
        {
            d3d12barrier.Flags |= D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
        }

        d3d12barrier.Transition.pResource = &r.d3dResource();
        d3d12barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d3d12barrier.Transition.StateBefore = getDx12GpuState(b.prevState);
        d3d12barrier.Transition.StateAfter  = getDx12GpuState(b.postState);
    }

    if (resultBarriers.empty())
        return;

    outList.ResourceBarrier((UINT)resultBarriers.size(), resultBarriers.data());
}

void Dx12WorkBundle::buildComputeCmd(const unsigned char* data, const AbiComputeCmd* computeCmd, const CommandInfo& cmdInfo, ID3D12GraphicsCommandList6& outList)
{
    Dx12ShaderDb& db = m_device.shaderDb();

    //TODO: optimize this resolve.
    db.resolve(computeCmd->shader);
    ID3D12PipelineState* pso = db.unsafeGetCsPso(computeCmd->shader);
    CPY_ASSERT(pso != nullptr);
    if (pso == nullptr)
        return;

    //TODO: set this once per cmd list
    outList.SetComputeRootSignature(&m_device.defaultComputeRootSignature());

    //TODO: prune this.
    outList.SetDescriptorHeaps(1, &m_srvUavTable.ownerHeap);

    outList.SetPipelineState(pso);

    //TODO: this can be taken outside
    if (computeCmd->inlineConstantBufferSize > 0)
    {
        CPY_ASSERT(computeCmd->inlineConstantBufferSize <= (m_uploadMemBlock.uploadSize - cmdInfo.uploadBufferOffset));
        memcpy((char*)m_uploadMemBlock.mappedBuffer + cmdInfo.uploadBufferOffset, computeCmd->inlineConstantBuffer.data(data), computeCmd->inlineConstantBufferSize);
        
        //HACK: dx12 requires aligned buffers to be 256, this size has been hacked to be aligned in WorkBundleDb, processCompte()
        UINT alignedSize = alignByte(computeCmd->inlineConstantBufferSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbufferDesc = { m_uploadMemBlock.gpuVA + cmdInfo.uploadBufferOffset, alignedSize };
        m_device.device().CreateConstantBufferView(&cbufferDesc, m_cbvTable.getCpuHandle(cmdInfo.constantBufferTableOffset));
        outList.SetComputeRootDescriptorTable(m_device.tableIndex(Dx12Device::TableTypes::Cbv, 0), m_cbvTable.getGpuHandle(cmdInfo.constantBufferTableOffset)); //one and only one table for cbv
    }
    else if (computeCmd->constantCounts > 0)
    {
        CPY_ASSERT(cmdInfo.constantBufferTableOffset >= 0);
        const Buffer* buffers = computeCmd->constants.data(data);
        for (int cbufferId = 0; cbufferId < computeCmd->constantCounts; ++cbufferId)
        {
            Dx12Resource& resource = m_device.resources().unsafeGetResource(buffers[cbufferId]);
            CPY_ASSERT(resource.isBuffer());
            Dx12Buffer& buff = (Dx12Buffer&)resource;
            m_device.device().CopyDescriptorsSimple(1, m_cbvTable.getCpuHandle(cbufferId + cmdInfo.constantBufferTableOffset), buff.cbv().handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        D3D12_GPU_DESCRIPTOR_HANDLE descriptor = m_cbvTable.getGpuHandle(cmdInfo.constantBufferTableOffset);
        outList.SetComputeRootDescriptorTable(m_device.tableIndex(Dx12Device::TableTypes::Cbv, 0), descriptor); //one and oonly one table for cbv
    }

    const InResourceTable* inTables = computeCmd->inResourceTables.data(data);
    for (int inTableId = 0; inTableId < computeCmd->inResourceTablesCounts; ++inTableId)
    {
        auto it = m_workBundle.tableAllocations.find((ResourceTable)inTables[inTableId]);
        CPY_ASSERT(it != m_workBundle.tableAllocations.end());
        if (it == m_workBundle.tableAllocations.end())
            return;

        D3D12_GPU_DESCRIPTOR_HANDLE descriptor = m_srvUavTable.getGpuHandle(it->second.offset);
        outList.SetComputeRootDescriptorTable(m_device.tableIndex(Dx12Device::TableTypes::Srv, inTableId) , descriptor);
    }

    const OutResourceTable* outTables = computeCmd->outResourceTables.data(data);
    for (int outTableId = 0; outTableId < computeCmd->outResourceTablesCounts; ++outTableId)
    {
        auto it = m_workBundle.tableAllocations.find((ResourceTable)outTables[outTableId]);
        CPY_ASSERT(it != m_workBundle.tableAllocations.end());
        if (it == m_workBundle.tableAllocations.end())
            return;

        D3D12_GPU_DESCRIPTOR_HANDLE descriptor = m_srvUavTable.getGpuHandle(it->second.offset);
        outList.SetComputeRootDescriptorTable(m_device.tableIndex(Dx12Device::TableTypes::Uav, outTableId) , descriptor);
    }

    outList.Dispatch(computeCmd->x, computeCmd->y, computeCmd->z);
}

void Dx12WorkBundle::buildCopyCmd(const unsigned char* data, const AbiCopyCmd* copyCmd, ID3D12GraphicsCommandList6& outList)
{
    Dx12ResourceCollection& resources = m_device.resources();
    Dx12Resource& src = resources.unsafeGetResource(copyCmd->source);
    Dx12Resource& dst = resources.unsafeGetResource(copyCmd->destination);
    outList.CopyResource(&dst.d3dResource(), &src.d3dResource());
}

void Dx12WorkBundle::buildDownloadCmd(
    const unsigned char* data, const AbiDownloadCmd* downloadCmd,
    const CommandInfo& cmdInfo, WorkType workType,
    ID3D12GraphicsCommandList6& outList)
{
    Dx12ResourceCollection& resources = m_device.resources();
    CPY_ASSERT(cmdInfo.commandDownloadIndex >= 0 && cmdInfo.commandDownloadIndex < (int)m_downloadStates.size());
    CPY_ASSERT(downloadCmd->source.valid());
    auto& downloadState = m_downloadStates[cmdInfo.commandDownloadIndex];
    downloadState.queueType = workType;
    downloadState.fenceValue = m_currentFenceValue + 1;
    downloadState.resource = downloadCmd->source;
    Dx12Resource& dx12Resource = resources.unsafeGetResource(downloadCmd->source);
    downloadState.mappedMemory = dx12Resource.mappedMemory();
    CPY_ASSERT(downloadState.mappedMemory != nullptr);
}

void Dx12WorkBundle::buildUploadCmd(const unsigned char* data, const AbiUploadCmd* uploadCmd, const CommandInfo& cmdInfo, ID3D12GraphicsCommandList6& outList)
{
    //TODO: this can be jobified.
    {
        CPY_ASSERT(cmdInfo.uploadBufferOffset < m_uploadMemBlock.uploadSize);
        CPY_ASSERT(uploadCmd->sourceSize <= (m_uploadMemBlock.uploadSize - cmdInfo.uploadBufferOffset));
        memcpy(((unsigned char*)m_uploadMemBlock.mappedBuffer) + cmdInfo.uploadBufferOffset, uploadCmd->sources.data(data), uploadCmd->sourceSize);
    }

    
    Dx12ResourceCollection& resources = m_device.resources();
    Dx12Resource& destinationResource = resources.unsafeGetResource(uploadCmd->destination);
    if (destinationResource.isBuffer())
    {
        outList.CopyBufferRegion(
            &destinationResource.d3dResource(),
            0ull,
            m_uploadMemBlock.buffer,
            m_uploadMemBlock.offset + cmdInfo.uploadBufferOffset,
            uploadCmd->sourceSize);
    }
    else
    {
        CPY_ASSERT_MSG(false, "upload command not ready for non buffer resources.");
    }

}

void Dx12WorkBundle::buildCommandList(int listIndex, const CommandList* cmdList, WorkType workType, ID3D12GraphicsCommandList6& outList)
{
    CPY_ASSERT(cmdList->isFinalized());
    const unsigned char* listData = cmdList->data();
    const ProcessedList& pl = m_workBundle.processedLists[listIndex];
    for (int commandIndex = 0; commandIndex < pl.commandSchedule.size(); ++commandIndex)
    {
        const CommandInfo& cmdInfo = pl.commandSchedule[commandIndex];
        const unsigned char* cmdBlob = listData + cmdInfo.commandOffset;
        AbiCmdTypes cmdType = *((AbiCmdTypes*)cmdBlob);
        applyBarriers(cmdInfo.preBarrier, outList);
        switch (cmdType)
        {
        case AbiCmdTypes::Compute:
            {
                const auto* abiCmd = (const AbiComputeCmd*)cmdBlob;
                buildComputeCmd(listData, abiCmd, cmdInfo, outList);
            }
            break;
        case AbiCmdTypes::Copy:
            {
                const auto* abiCmd = (const AbiCopyCmd*)cmdBlob;
                buildCopyCmd(listData, abiCmd, outList);
            }
            break;
        case AbiCmdTypes::Upload:
            {
                const auto* abiCmd = (const AbiUploadCmd*)cmdBlob;
                buildUploadCmd(listData, abiCmd, cmdInfo, outList);
            }
            break;
        case AbiCmdTypes::Download:
            {
                const auto* abiCmd = (const AbiDownloadCmd*)cmdBlob;
                buildDownloadCmd(listData, abiCmd, cmdInfo, workType, outList);
            }
            break;
        default:
            CPY_ASSERT_FMT(false, "Unrecognized serialized command %d", cmdType);
            return;
        }
        applyBarriers(cmdInfo.postBarrier, outList);
    }

    outList.Close();
}

UINT64 Dx12WorkBundle::execute(CommandList** commandLists, int commandListsCount)
{
    CPY_ASSERT(commandListsCount == (int)m_workBundle.processedLists.size());

    WorkType workType = WorkType::Graphics;
    auto& queues = m_device.queues();
    ID3D12CommandQueue& cmdQueue = queues.cmdQueue(workType);
    Dx12MemoryPools& pools = queues.memPools(workType);

    pools.uploadPool->beginUsage();
    pools.tablePool->beginUsage();
    m_downloadStates.resize((int)m_workBundle.resourcesToDownload.size());

    if (m_workBundle.totalUploadBufferSize)
        m_uploadMemBlock = pools.uploadPool->allocUploadBlock(m_workBundle.totalUploadBufferSize);

    if ((m_workBundle.totalTableSize + m_workBundle.totalConstantBuffers) > 0)
    {
        Dx12GpuDescriptorTable table = pools.tablePool->allocateTable(m_workBundle.totalTableSize + m_workBundle.totalConstantBuffers);
        m_srvUavTable = table;
        table.advance(m_workBundle.totalTableSize);
        m_cbvTable = table;
    }

    uploadAllTables();

    std::vector<Dx12List> lists;
    std::vector<ID3D12CommandList*> dx12Lists;
    m_currentFenceValue = queues.currentFenceValue(workType);
    for (int i = 0; i < commandListsCount; ++i)
    {
        lists.emplace_back();
        Dx12List& list = lists.back();
        queues.allocate(workType, list);
        dx12Lists.push_back(list.list);

        buildCommandList(i, commandLists[i], workType, *list.list);
    }

    cmdQueue.ExecuteCommandLists(dx12Lists.size(), dx12Lists.data());

    UINT64 fenceVal = queues.signalFence(workType);
    for (auto l : lists)
        queues.deallocate(l, fenceVal);

    pools.tablePool->endUsage();
    pools.uploadPool->endUsage();
    return fenceVal;
}

void Dx12WorkBundle::getDownloadResourceMap(Dx12DownloadResourceMap& downloadMap)
{
    for (auto& state : m_downloadStates)
        downloadMap[state.resource] = state;
}

}
}