module;

#include <windows.h>
#include <windowsx.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include "d3dx12.h"
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <DirectXColors.h>
#include <DirectXCollision.h>
#include <unknwn.h>
#include <winrt/base.h> // com_ptr?
#include <filesystem>

export module d3d_util;

using winrt::com_ptr;
using winrt::check_hresult;

namespace d3d_util {

export uint32_t calc_constant_buffer_byte_size(UINT byteSize)
{
    // Constant buffers must be a multiple of the minimum hardware
    // allocation size (usually 256 bytes).  So round up to nearest
    // multiple of 256.  We do this by adding 255 and then masking off
    // the lower 2 bytes which store all bits < 256.
    // Example: Suppose byteSize = 300.
    // (300 + 255) & ~255
    // 555 & ~255
    // 0x022B & ~0x00ff
    // 0x022B & 0xff00
    // 0x0200
    // 512
    return (byteSize + 255) & ~255;
}

// Simpler version of thing below. Just create the TYPE_DEFAULT buffer. No upload buffer at same time.
export com_ptr<ID3D12Resource> create_default_buffer(ID3D12Device* device, uint64_t size) {
    com_ptr<ID3D12Resource> buffer;

    CD3DX12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE_DEFAULT);
    auto res_desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    auto hr = device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &res_desc, 
                                              D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, __uuidof(buffer), buffer.put_void());
    check_hresult(hr);
    return buffer;
}

export com_ptr<ID3D12Resource> create_upload_buffer(ID3D12Device* device, uint64_t size) {
    com_ptr<ID3D12Resource> buffer;
    // According to the documentation for the heap type D3D12_HEAP_TYPE_UPLOAD, resources in that heap 
    // must be created with state D3D12_RESOURCE_STATE_GENERIC_READ, and stay that way.
    CD3DX12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE_UPLOAD);
    auto res_desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    auto hr = device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &res_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, __uuidof(buffer), buffer.put_void());
    check_hresult(hr);
    return buffer;
}

// copied from book example code
export
com_ptr<ID3D12Resource> create_default_buffer(ID3D12Device* device,
                                              ID3D12GraphicsCommandList* cmdList,
                                              const void* initData,
                                              uint64_t byteSize,
                                              com_ptr<ID3D12Resource>& uploadBuffer) {

    com_ptr<ID3D12Resource> defaultBuffer;

    CD3DX12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE_DEFAULT);
    auto res_desc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
    // Create the actual default buffer resource.
    auto hr = device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &res_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        __uuidof(defaultBuffer), defaultBuffer.put_void());
    check_hresult(hr);

    CD3DX12_HEAP_PROPERTIES upload_heap_props(D3D12_HEAP_TYPE_UPLOAD);
    // In order to copy CPU memory data into our default buffer, we need to create
    // an intermediate upload heap. 
    check_hresult(device->CreateCommittedResource(
        &upload_heap_props,
        D3D12_HEAP_FLAG_NONE,
        &res_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        __uuidof(uploadBuffer), uploadBuffer.put_void()));


    // Describe the data we want to copy into the default buffer.
    D3D12_SUBRESOURCE_DATA subResourceData = {};
    subResourceData.pData = initData;
    subResourceData.RowPitch = byteSize;
    subResourceData.SlicePitch = subResourceData.RowPitch;

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    // Schedule to copy the data to the default buffer resource.  At a high level, the helper function UpdateSubresources
    // will copy the CPU memory into the intermediate upload heap.  Then, using ID3D12CommandList::CopySubresourceRegion,
    // the intermediate upload heap data will be copied to mBuffer.
    cmdList->ResourceBarrier(1, &barrier);
    UpdateSubresources<1>(cmdList, defaultBuffer.get(), uploadBuffer.get(), 0, 0, 1, &subResourceData);
    cmdList->ResourceBarrier(1, &barrier2);

    // Note: uploadBuffer has to be kept alive after the above function calls because
    // the command list has not been executed yet that performs the actual copy.
    // The caller can Release the uploadBuffer after it knows the copy has been executed.

    return defaultBuffer;
}

export
com_ptr<ID3DBlob> compile_shader(const std::wstring &filename, const D3D_SHADER_MACRO *defines,
                                 const std::string &entrypoint, const std::string &target) {

    uint32_t compileFlags = 0;
#if defined(DEBUG) || defined(_DEBUG)  
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = S_OK;

    com_ptr<ID3DBlob> byteCode = nullptr;
    com_ptr<ID3DBlob> errors;
    auto p = std::filesystem::current_path();
    hr = D3DCompileFromFile(filename.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entrypoint.c_str(),
                            target.c_str(), compileFlags, 0, byteCode.put(), errors.put());

    if (errors != nullptr) {
        OutputDebugStringA((char*)errors->GetBufferPointer());
    }

    check_hresult(hr);

    return byteCode;
}

export template<typename T>
class UploadBuffer {
public:
    UploadBuffer(ID3D12Device* device, uint32_t elementCount, bool isConstantBuffer) :
        mIsConstantBuffer(isConstantBuffer)
    {
        mElementByteSize = sizeof(T);

        // Constant buffer elements need to be multiples of 256 bytes.
        // This is because the hardware can only view constant data 
        // at m*256 byte offsets and of n*256 byte lengths. 
        // typedef struct D3D12_CONSTANT_BUFFER_VIEW_DESC {
        // UINT64 OffsetInBytes; // multiple of 256
        // UINT   SizeInBytes;   // multiple of 256
        // } D3D12_CONSTANT_BUFFER_VIEW_DESC;
        if (isConstantBuffer) {
            mElementByteSize = calc_constant_buffer_byte_size(sizeof(T));
        }

        auto heap_props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto res_desc = CD3DX12_RESOURCE_DESC::Buffer(mElementByteSize * elementCount);
        check_hresult(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &res_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_upload_buffer)));

        check_hresult(m_upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedData)));

        // We do not need to unmap until we are done with the resource.  However, we must not write to
        // the resource while it is in use by the GPU (so we must use synchronization techniques).
    }

    UploadBuffer(const UploadBuffer& rhs) = delete;
    UploadBuffer& operator=(const UploadBuffer& rhs) = delete;
    ~UploadBuffer() {
        if (m_upload_buffer != nullptr)
            m_upload_buffer->Unmap(0, nullptr);

        mMappedData = nullptr;
    }

    ID3D12Resource* resource() const {
        return m_upload_buffer.get();
    }

    void copy_data(int elementIndex, const T& data) {
        memcpy(&mMappedData[elementIndex * mElementByteSize], &data, sizeof(T));
    }

private:
    com_ptr<ID3D12Resource> m_upload_buffer;
    BYTE* mMappedData = nullptr;

    UINT mElementByteSize = 0;
    bool mIsConstantBuffer = false;
};

}