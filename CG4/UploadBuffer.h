#pragma once
#include "Common.h"
#include "Dx12Helpers.h"

template<typename T>
class UploadBuffer
{
public:
    UploadBuffer(ID3D12Device* device, UINT elementCount, bool isConstantBuffer)
    {
        mElementByteSize = sizeof(T);
        if(isConstantBuffer)
            mElementByteSize = Align256(mElementByteSize);

        const UINT64 bufferBytes = static_cast<UINT64>(mElementByteSize) * elementCount;

        // сохраняем в локальные переменные
        auto heapProps  = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bufferDesc = BufferDesc(bufferBytes);

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&mUploadBuffer)));

        ThrowIfFailed(mUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedData)));
    }

    ~UploadBuffer()
    {
        if(mUploadBuffer) mUploadBuffer->Unmap(0, nullptr);
        mMappedData = nullptr;
    }

    UploadBuffer(const UploadBuffer&) = delete;
    UploadBuffer& operator=(const UploadBuffer&) = delete;

    ID3D12Resource* Resource() const { return mUploadBuffer.Get(); }

    void CopyData(int elementIndex, const T& data)
    {
        memcpy(mMappedData + static_cast<size_t>(elementIndex) * mElementByteSize, &data, sizeof(T));
    }

private:
    ComPtr<ID3D12Resource> mUploadBuffer;
    uint8_t* mMappedData = nullptr;
    UINT mElementByteSize = 0;
};
