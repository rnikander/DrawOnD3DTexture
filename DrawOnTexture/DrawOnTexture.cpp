// DrawOnTexture.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "DrawOnTexture.h"

#include <cassert>
#include <string>
#include <format>
#include <unordered_map>
#include <filesystem>

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
#include <winrt/base.h> 
#include <d2d1_1.h>
#include <pix3.h>
#include <wincodec.h> // Windows Image Component ?
#include <WICTextureLoader.h>
#include <ResourceUploadBatch.h>
// D11 on D12
#include <d3d11on12.h>
// Direct 2D and DirectWrite
#include <dwrite.h>
#include <d2d1.h>
#include <d2d1_3.h>


import d3d_util;

using winrt::com_ptr;
using winrt::check_hresult;
namespace Colors = DirectX::Colors;
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
using DirectX::XMFLOAT4X4;
using DirectX::XMVECTOR;
using DirectX::XMMATRIX;

// Toggle this to use texture from file, or draw the texture using Direct2D.
const bool use_texture_from_file = false;

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

uint64_t draw_count = 0;

// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 611; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = (char*)u8".\\D3D12\\"; }

// Convenience
template <typename... Args>
void debugf(std::basic_format_string<wchar_t, std::type_identity_t<Args>...> fmt, Args && ...args) {
    OutputDebugString(std::format(fmt, std::forward<Args>(args)...).c_str());
}

DirectX::XMFLOAT4X4 Identity4x4()
{
    static DirectX::XMFLOAT4X4 I(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    return I;
}

struct ObjectConstants {
    XMFLOAT4X4 WorldViewProj = Identity4x4();
};

struct Vertex {
    XMFLOAT2 pos;
    XMFLOAT2 texc;

    template <class It> It format_to(It out) const {
        const char* delim = "<Vertex (";
        out = std::format_to(out, "{},", pos.x);
        out = std::format_to(out, "{})>", pos.y);
        return out;
    }
};

struct SubmeshGeometry {
    uint32_t index_count = 0;
    uint32_t start_index = 0;
    int32_t base_vertex = 0;
};

struct MeshGeometry {
    // Give it a name so we can look it up by name.
    std::string name;

    // System memory copies.  Use Blobs because the vertex/index format can be various things.
    com_ptr<ID3DBlob> vbuf_cpu = nullptr;
    com_ptr<ID3DBlob> ibuf_cpu = nullptr;

    com_ptr<ID3D12Resource> vbuf_gpu = nullptr;
    com_ptr<ID3D12Resource> ibuf_gpu = nullptr;

    com_ptr<ID3D12Resource> vbuf_uploader = nullptr;
    com_ptr<ID3D12Resource> ibuf_uploader = nullptr;

    // Data about the buffers.
    uint32_t vertex_stride = 0;
    uint32_t vbuf_size = 0;
    DXGI_FORMAT index_format = DXGI_FORMAT_R16_UINT;
    uint32_t ibuf_size = 0;
    uint32_t index_count;

    // A MeshGeometry may store multiple geometries in one vertex/index buffer.
    // Use this container to define the Submesh geometries so we can draw
    // the Submeshes individually.
    std::unordered_map<std::string, SubmeshGeometry> parts;

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view() const {
        D3D12_VERTEX_BUFFER_VIEW vbv;
        vbv.BufferLocation = vbuf_gpu->GetGPUVirtualAddress();
        vbv.StrideInBytes = vertex_stride;
        vbv.SizeInBytes = vbuf_size;
        return vbv;
    }

    D3D12_INDEX_BUFFER_VIEW index_buffer_view() const {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = ibuf_gpu->GetGPUVirtualAddress();
        ibv.Format = index_format;
        ibv.SizeInBytes = ibuf_size;
        return ibv;
    }

    // We can free this memory after we finish upload to the GPU.
    void dispose_uploaders() {
        vbuf_uploader = nullptr;
        ibuf_uploader = nullptr;
    }
};

class App {
    int m_client_width = 800;
    int m_client_height = 600;

    static App* s_app;
    HINSTANCE m_app_h = nullptr;           // application instance handle
    HWND      m_main_window_h = nullptr;   // main window handle
    bool m_needs_draw = false;

    // Bumped it to version 3 from 1 (?).
    com_ptr<ID2D1Factory3> m_d2d_factory;

    com_ptr<IDXGIFactory4> m_dxgi_factory;
    com_ptr<IDXGISwapChain> m_swap_chain;
    com_ptr<ID3D12Device> m_device;

    com_ptr<ID3D12Fence> m_fence;
    UINT64 m_current_fence = 0;

    com_ptr<ID3D12CommandQueue> m_command_queue;
    com_ptr<ID3D12CommandAllocator> m_direct_cmd_list_alloc;
    com_ptr<ID3D12GraphicsCommandList> m_command_list;

    uint32_t m_rtv_desc_size = 0;
    uint32_t m_dsv_desc_size = 0;
    uint32_t m_cbv_srv_uav_desc_size = 0;

    com_ptr<ID3D12DescriptorHeap> m_rtv_heap;
    com_ptr<ID3D12DescriptorHeap> m_dsv_heap;
    com_ptr<ID3D12DescriptorHeap> m_cbv_heap;

    D3D_DRIVER_TYPE m_d3d_driver_type = D3D_DRIVER_TYPE_HARDWARE;
    DXGI_FORMAT m_back_buffer_format = DXGI_FORMAT_R8G8B8A8_UNORM; // ??
    DXGI_FORMAT m_depth_stencil_format = DXGI_FORMAT_D24_UNORM_S8_UINT; // ??

    bool m_4x_msaa_state = false;        // 4X MSAA enabled
    uint32_t m_4x_msaa_quality = 0;      // quality level of 4X MSAA

    static const int SwapChainBufferCount = 2;
    int m_curr_back_buffer = 0;
    com_ptr<ID3D12Resource> m_swap_chain_buffer[SwapChainBufferCount];
    com_ptr<ID3D12Resource> m_depth_stencil_buffer;

    com_ptr<ID3D12PipelineState> m_pso;
    com_ptr<ID3D12RootSignature> m_root_signature;

    D3D12_VIEWPORT m_screen_viewport;
    D3D12_RECT m_scissor_rect;

    std::unique_ptr<MeshGeometry> m_geo;
    com_ptr<ID3D12Resource> m_texture1;
    com_ptr<ID3D12Resource> m_texture2;

    XMFLOAT4X4 m_world = Identity4x4();
    XMFLOAT4X4 m_view = Identity4x4();
    XMFLOAT4X4 m_proj = Identity4x4();

    std::unique_ptr<d3d_util::UploadBuffer<ObjectConstants>> m_object_cb = nullptr;
    struct ShaderByteCode {
        com_ptr<ID3DBlob> vs = nullptr;
        com_ptr<ID3DBlob> ps = nullptr;
       /* com_ptr<ID3DBlob> vs2 = nullptr;
        com_ptr<ID3DBlob> ps2 = nullptr;*/
    };
    ShaderByteCode m_shader_byte_code;
    std::vector<D3D12_INPUT_ELEMENT_DESC> m_input_layout;

    // 11 on 12 stuff
    com_ptr<ID3D11Device> m_d3d11_device;
    com_ptr<ID3D11On12Device> m_11on12_device;
    com_ptr<ID2D1Device2> m_d2d_device;
    com_ptr<ID2D1DeviceContext2> m_d2d_device_context;
    com_ptr<IDWriteFactory> m_dwrite_factory;
    com_ptr<ID3D11Resource> m_d11_texture;  // corresponds to texture2
    com_ptr<ID2D1SolidColorBrush> m_red_brush;
    com_ptr<ID2D1RenderTarget> m_render_target;

public:

    static App* shared() {
        return s_app;
    }

    App(HINSTANCE hInstance)
        :m_app_h(hInstance)
    {
        assert(s_app == nullptr);
        s_app = this;
    }

    LRESULT msg_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        // debugf(L"msg_proc {}\n", msg);
        switch (msg)
        {
        case WM_KEYDOWN:
            m_needs_draw = true;
            break;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    int run() {
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            DispatchMessage(&msg);
            if (m_needs_draw) {
                update();
                draw();
            }
        }
        return (int)msg.wParam;
    }

    bool init_main_window() {
        WNDCLASS wc{};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MainWndProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = m_app_h;
        wc.hIcon = LoadIcon(0, IDI_APPLICATION);
        wc.hCursor = LoadCursor(0, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
        wc.lpszMenuName = 0;
        wc.lpszClassName = L"MainWnd";

        if (!RegisterClass(&wc))
        {
            MessageBox(0, L"RegisterClass Failed.", 0, 0);
            return false;
        }

        // Compute window rectangle dimensions based on requested client area dimensions.
        RECT R = { 0, 0, m_client_width, m_client_height };
        AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, false);
        int width = R.right - R.left;
        int height = R.bottom - R.top;

        std::wstring caption = L"Draw On Texture";

        m_main_window_h = CreateWindow(L"MainWnd", caption.c_str(),
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, 0, 0, m_app_h, 0);
        if (!m_main_window_h)
        {
            MessageBox(0, L"CreateWindow Failed.", 0, 0);
            return false;
        }

        // https://learn.microsoft.com/en-us/windows/win32/wintouch/getting-started-with-multi-touch-messages
        RegisterTouchWindow(m_main_window_h, 0);

        ShowWindow(m_main_window_h, SW_SHOW);
        UpdateWindow(m_main_window_h);

        return true;
    }

private:
    void create_d2d_factory() {
        D2D1_FACTORY_TYPE type = D2D1_FACTORY_TYPE_SINGLE_THREADED;
        D2D1_FACTORY_OPTIONS options = {};
        options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
        auto hr = D2D1CreateFactory(type, __uuidof(ID2D1Factory3), &options, m_d2d_factory.put_void());
        check_hresult(hr);
    }

    void create_debug_layer() {
        com_ptr<ID3D12Debug> debugController;
        check_hresult(D3D12GetDebugInterface(__uuidof(debugController), debugController.put_void()));
        debugController->EnableDebugLayer();
    }
    void create_dxgi_factory() {
        check_hresult(CreateDXGIFactory1(__uuidof(m_dxgi_factory), m_dxgi_factory.put_void()));
    }
    void create_d3d12_device() {
        // Try to create hardware device.
        IUnknown *default_adapter = nullptr;
        HRESULT hr = D3D12CreateDevice(default_adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(m_device), m_device.put_void());
        // Fallback to the "WARP" software device.
        if (FAILED(hr)) {
            com_ptr<IDXGIAdapter> warp_adapter;
            check_hresult(m_dxgi_factory->EnumWarpAdapter(__uuidof(warp_adapter), warp_adapter.put_void()));
            check_hresult(D3D12CreateDevice(warp_adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(m_device), m_device.put_void()));
        }
    }

    void create_fence() {
        check_hresult(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(m_fence), m_fence.put_void()));
    }

    void get_descriptor_sizes() {
        m_rtv_desc_size = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_dsv_desc_size = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        m_cbv_srv_uav_desc_size = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    
    void check_msaa_support() {
        // Check 4X MSAA quality support for our back buffer format.
        // All Direct3D 11 capable devices support 4X MSAA for all render target formats, so we only need to check quality support.

        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
        levels.Format = m_back_buffer_format;
        levels.SampleCount = 4;
        levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        levels.NumQualityLevels = 0;
        auto hr = m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels));
        check_hresult(hr);

        m_4x_msaa_quality = levels.NumQualityLevels;
        assert(m_4x_msaa_quality > 0 && "Unexpected MSAA quality level.");
    }

public:
    void init_directx() {
        create_d2d_factory();
        create_debug_layer();
        create_dxgi_factory();
        create_d3d12_device();
        create_fence();
        get_descriptor_sizes();
        check_msaa_support();
        // log_adapters();
        create_command_objects();
        create_swap_chain();
        create_rtv_and_dsv_descriptor_heaps();

        // Reset the command list to prep for initialization commands.
        check_hresult(m_command_list->Reset(m_direct_cmd_list_alloc.get(), nullptr));

        load_textures();
        build_descriptor_heaps();
        build_constant_buffers();
        build_root_signature();
        build_shaders_and_input_layout();
        make_geo();
        build_pso();

        // Execute the initialization commands.
        check_hresult(m_command_list->Close());
        ID3D12CommandList* cmdsLists[] = { m_command_list.get() };
        m_command_queue->ExecuteCommandLists(1, cmdsLists);

        // Wait until initialization is complete.
        flush_command_queue();
        debugf(L"finished init_directx()\n");
    }


    void on_resize() {
        assert(m_device);
        assert(m_swap_chain);
        assert(m_direct_cmd_list_alloc);

        // Flush before changing any resources.
        flush_command_queue();

        check_hresult(m_command_list->Reset(m_direct_cmd_list_alloc.get(), nullptr));

        // Release the previous resources we will be recreating.
        for (int i = 0; i < SwapChainBufferCount; ++i) {
            m_swap_chain_buffer[i] = nullptr; // .Reset();
        }
        m_depth_stencil_buffer = nullptr; //.Reset();

        // Resize the swap chain.
        check_hresult(m_swap_chain->ResizeBuffers(
            SwapChainBufferCount,
            m_client_width, m_client_height,
            m_back_buffer_format,
            DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

        m_curr_back_buffer = 0;

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(m_rtv_heap->GetCPUDescriptorHandleForHeapStart());
        for (uint32_t i = 0; i < SwapChainBufferCount; i++) {
            check_hresult(m_swap_chain->GetBuffer(i, IID_PPV_ARGS(&m_swap_chain_buffer[i])));
            m_device->CreateRenderTargetView(m_swap_chain_buffer[i].get(), nullptr, rtvHeapHandle);
            rtvHeapHandle.Offset(1, m_rtv_desc_size);
        }

        // Create the depth/stencil buffer and view.
        D3D12_RESOURCE_DESC depthStencilDesc;
        depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthStencilDesc.Alignment = 0;
        depthStencilDesc.Width = m_client_width;
        depthStencilDesc.Height = m_client_height;
        depthStencilDesc.DepthOrArraySize = 1;
        depthStencilDesc.MipLevels = 1;

        // Correction 11/12/2016: SSAO chapter requires an SRV to the depth buffer to read from 
        // the depth buffer.  Therefore, because we need to create two views to the same resource:
        //   1. SRV format: DXGI_FORMAT_R24_UNORM_X8_TYPELESS
        //   2. DSV Format: DXGI_FORMAT_D24_UNORM_S8_UINT
        // we need to create the depth buffer resource with a typeless format.  
        depthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;

        depthStencilDesc.SampleDesc.Count = m_4x_msaa_state ? 4 : 1;
        depthStencilDesc.SampleDesc.Quality = m_4x_msaa_state ? (m_4x_msaa_quality - 1) : 0;
        depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE optClear;
        optClear.Format = m_depth_stencil_format;
        optClear.DepthStencil.Depth = 1.0f;
        optClear.DepthStencil.Stencil = 0;
        CD3DX12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE_DEFAULT);
        check_hresult(m_device->CreateCommittedResource(&heap_props,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &depthStencilDesc,
                                                        D3D12_RESOURCE_STATE_COMMON,
                                                        &optClear, __uuidof(m_depth_stencil_buffer), m_depth_stencil_buffer.put_void()));

        // Create descriptor to mip level 0 of entire resource using the format of the resource.
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Format = m_depth_stencil_format;
        dsvDesc.Texture2D.MipSlice = 0;
        m_device->CreateDepthStencilView(m_depth_stencil_buffer.get(), &dsvDesc, depth_stencil_view());

        // Transition the resource from its initial state to be used as a depth buffer.
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_depth_stencil_buffer.get(),
                                                            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_command_list->ResourceBarrier(1, &barrier);

        // Execute the resize commands.
        check_hresult(m_command_list->Close());
        ID3D12CommandList* cmdsLists[] = { m_command_list.get() };
        m_command_queue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

        // Wait until resize is complete.
        flush_command_queue();

        // Update the viewport transform to cover the client area.
        m_screen_viewport.TopLeftX = 0;
        m_screen_viewport.TopLeftY = 0;
        m_screen_viewport.Width = static_cast<float>(m_client_width);
        m_screen_viewport.Height = static_cast<float>(m_client_height);
        m_screen_viewport.MinDepth = 0.0f;
        m_screen_viewport.MaxDepth = 1.0f;

        m_scissor_rect = { 0, 0, m_client_width, m_client_height };

        // DirectX::XMMATRIX P = DirectX::XMMatrixPerspectiveFovLH(0.25f * pi, aspect_ratio(), 1.0f, 1000.0f);
        auto P = DirectX::XMMatrixOrthographicLH(2, 2, -0.5, 1000.0f);
        XMStoreFloat4x4(&m_proj, P);
    }


    // Initializes m_rtv_heap and m_dsv_heap (render target, depth stencil)
    void create_rtv_and_dsv_descriptor_heaps() {
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
        rtv_heap_desc.NumDescriptors = SwapChainBufferCount;
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        rtv_heap_desc.NodeMask = 0;
        check_hresult(m_device->CreateDescriptorHeap(&rtv_heap_desc, __uuidof(m_rtv_heap), m_rtv_heap.put_void()));
        debugf(L"Created RTV descriptor heap, with space for {} descriptors.\n", SwapChainBufferCount);

        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        dsv_heap_desc.NodeMask = 0;
        check_hresult(m_device->CreateDescriptorHeap(&dsv_heap_desc, __uuidof(m_dsv_heap), m_dsv_heap.put_void()));
    }

    void create_command_objects() {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        check_hresult(m_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&m_command_queue)));

        check_hresult(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(m_direct_cmd_list_alloc), m_direct_cmd_list_alloc.put_void()));

        check_hresult(m_device->CreateCommandList(0,
                                                  D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  m_direct_cmd_list_alloc.get(), // Associated command allocator
                                                  nullptr,                   // Initial PipelineStateObject
                                                  __uuidof(m_command_list), m_command_list.put_void()));

        // Start off in a closed state.  This is because the first time we refer to the command list we will Reset it, and it needs 
        // to be closed before calling Reset.
        m_command_list->Close();
    }

    void create_swap_chain() {
        debugf(L"create_swap_chain {}x{}", m_client_width, m_client_height);
        // Release the previous swapchain we will be recreating.
        m_swap_chain = nullptr;

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = m_client_width;
        sd.BufferDesc.Height = m_client_height;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferDesc.Format = m_back_buffer_format;
        sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        sd.SampleDesc.Count = m_4x_msaa_state ? 4 : 1;
        sd.SampleDesc.Quality = m_4x_msaa_state ? (m_4x_msaa_quality - 1) : 0;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = SwapChainBufferCount;
        sd.OutputWindow = m_main_window_h;
        sd.Windowed = true;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

        // Note: Swap chain uses queue to perform flush.
        check_hresult(m_dxgi_factory->CreateSwapChain(m_command_queue.get(), &sd, m_swap_chain.put()));
    }

    ID3D12Resource* current_back_buffer() const {
        return m_swap_chain_buffer[m_curr_back_buffer].get();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE current_back_buffer_view() const {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtv_heap->GetCPUDescriptorHandleForHeapStart(), m_curr_back_buffer, m_rtv_desc_size);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil_view() const {
        return m_dsv_heap->GetCPUDescriptorHandleForHeapStart();
    }

    void load_textures();
    void build_descriptor_heaps();
    void build_constant_buffers();
    void build_root_signature();
    void build_shaders_and_input_layout();
    void make_geo();
    void build_pso();
    com_ptr<ID3D12Resource> draw_on_texture();

    void update();
    void draw();
    void flush_command_queue();
    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> get_static_samplers();

};

App* App::s_app = nullptr;


LRESULT CALLBACK
MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Forward hwnd on because we can get messages (e.g., WM_CREATE)
    // before CreateWindow returns, and thus before m_main_window_h is valid.
    return App::shared()->msg_proc(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    PIXLoadLatestWinPixGpuCapturerLibrary();

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: Place code here.

    App app(hInstance);
    app.init_main_window();
    app.init_directx();
    app.on_resize();
    return app.run();
}


void App::update() {
    // Convert Spherical to Cartesian coordinates.
    float x, y, z;
    x = 0;
    y = 0;
    z = -1;
    // Build the view matrix.
    XMVECTOR pos = DirectX::XMVectorSet(x, y, z, 1.0f);
    XMVECTOR target = DirectX::XMVectorZero();
    XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = DirectX::XMMatrixLookAtLH(pos, target, up);
    XMStoreFloat4x4(&m_view, view);

    XMMATRIX world = DirectX::XMLoadFloat4x4(&m_world);
    XMMATRIX proj = DirectX::XMLoadFloat4x4(&m_proj);
    XMMATRIX worldViewProj = world * view * proj;

    // Update the constant buffer with the latest worldViewProj matrix.
    ObjectConstants objConstants;
    XMStoreFloat4x4(&objConstants.WorldViewProj, XMMatrixTranspose(worldViewProj));
    m_object_cb->copy_data(0, objConstants);
}


void App::draw() {
    draw_count++;
    debugf(L"App::draw() {}\n", draw_count);

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    check_hresult(m_direct_cmd_list_alloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    check_hresult(m_command_list->Reset(m_direct_cmd_list_alloc.get(), m_pso.get()));
    PIXSetMarker(m_command_list.get(), 0xFF00FF00, "Draw count=%d", draw_count);
    m_command_list->RSSetViewports(1, &m_screen_viewport);
    m_command_list->RSSetScissorRects(1, &m_scissor_rect);

    // Indicate a state transition on the resource usage.
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(current_back_buffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_command_list->ResourceBarrier(1, &barrier);

    // Clear the back buffer and depth buffer.
    m_command_list->ClearRenderTargetView(current_back_buffer_view(), Colors::LightSteelBlue, 0, nullptr);
    m_command_list->ClearDepthStencilView(depth_stencil_view(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    auto bbv = current_back_buffer_view();
    auto dsv = depth_stencil_view();
    m_command_list->OMSetRenderTargets(1, &bbv, true, &dsv);

    ID3D12DescriptorHeap* desc_heaps[] = { m_cbv_heap.get() };
    m_command_list->SetDescriptorHeaps(1, desc_heaps);

    m_command_list->SetPipelineState(m_pso.get());
    m_command_list->SetGraphicsRootSignature(m_root_signature.get());

    auto vb_view = m_geo->vertex_buffer_view();
    auto ib_view = m_geo->index_buffer_view();
    m_command_list->IASetVertexBuffers(0, 1, &vb_view);
    m_command_list->IASetIndexBuffer(&ib_view);
    m_command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    CD3DX12_GPU_DESCRIPTOR_HANDLE heap_handle( m_cbv_heap->GetGPUDescriptorHandleForHeapStart() );
    m_command_list->SetGraphicsRootDescriptorTable(0, heap_handle);
    heap_handle.Offset(1 * m_cbv_srv_uav_desc_size);
    m_command_list->SetGraphicsRootDescriptorTable(1, heap_handle);
    debugf(L"index count = {}\n", m_geo->index_count);
    m_command_list->DrawIndexedInstanced(m_geo->index_count, 1, 0, 0, 0);
    
    // Indicate a state transition on the resource usage.
    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(current_back_buffer(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                         D3D12_RESOURCE_STATE_PRESENT);
    m_command_list->ResourceBarrier(1, &barrier2);

    // Done recording commands.
    check_hresult(m_command_list->Close());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmd_lists[] = { m_command_list.get() };
    m_command_queue->ExecuteCommandLists(1, cmd_lists);

    // swap the back and front buffers
    check_hresult(m_swap_chain->Present(0, 0));
    m_curr_back_buffer = (m_curr_back_buffer + 1) % SwapChainBufferCount;

    // Wait until frame commands are complete.  This waiting is inefficient and is done for simplicity. 
    // Later we will show how to organize our rendering code so we do not have to wait per frame.
    flush_command_queue();
    m_needs_draw = false;
    
}


void App::flush_command_queue() {
    // Advance the fence value to mark commands up to this fence point.
    m_current_fence++;

    // Add an instruction to the command queue to set a new fence point.  Because we 
    // are on the GPU timeline, the new fence point won't be set until the GPU finishes
    // processing all the commands prior to this Signal().
    check_hresult(m_command_queue->Signal(m_fence.get(), m_current_fence));

    // Wait until the GPU has completed commands up to this fence point.
    if (m_fence->GetCompletedValue() < m_current_fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, L"", false, EVENT_ALL_ACCESS);

        // Fire event when GPU hits current fence.  
        check_hresult(m_fence->SetEventOnCompletion(m_current_fence, eventHandle));

        // Wait until the GPU hits current fence event is fired.
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }
}


void App::load_textures() {
    // from MS docs on WIC
    // Create a decoder
    IWICImagingFactory* m_pIWICFactory;
    HRESULT hr = S_OK;

    // Create WIC factory
   /* hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_pIWICFactory));


    IWICBitmapDecoder* pDecoder = NULL;
    auto filename = L"kitten1.jpg";
    hr = m_pIWICFactory->CreateDecoderFromFilename(filename,                       // Image to be decoded
        NULL,                           // Do not prefer a particular vendor
        GENERIC_READ,                   // Desired read access to the file
        WICDecodeMetadataCacheOnDemand, // Cache metadata when needed
        &pDecoder                       // Pointer to the decoder
    );
    check_hresult(hr);
    // Retrieve the first frame of the image from the decoder
    IWICBitmapFrameDecode* pFrame = NULL;

    if (SUCCEEDED(hr)) {
        hr = pDecoder->GetFrame(0, &pFrame);
    } */

    com_ptr<ID3D12Resource> tex;
    DirectX::ResourceUploadBatch resourceUpload(m_device.get());
    resourceUpload.Begin();
    auto cwd = std::filesystem::current_path();
    hr = CreateWICTextureFromFile(m_device.get(), resourceUpload, L"kitten1b.jpg", tex.put(), false);
    check_hresult(hr);
    auto uploadResourcesFinished = resourceUpload.End(m_command_queue.get()); // ->GetCommandQueue());

    uploadResourcesFinished.wait();

    m_texture1 = std::move(tex);

    m_texture2 = draw_on_texture();
   /* dw_help = new DWriteHelper(m_device.get(), m_command_queue.get(), m_main_window_h);
    dw_help->write_text();
    m_text_texture = dw_help->get_texture();*/
}


// The base class created RTV and DSV descriptor heaps. This creates  CBV/SRV/UAV heap.
void App::build_descriptor_heaps() {
    // cbv = constant buffer view .... "view" ~ descriptor
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    heap_desc.NumDescriptors = 2;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;
    check_hresult(m_device->CreateDescriptorHeap(&heap_desc, __uuidof(m_cbv_heap), m_cbv_heap.put_void()));

    // int desc_size = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    //
    // Fill out the heap with actual descriptors.
    //
    CD3DX12_CPU_DESCRIPTOR_HANDLE desc_h(m_cbv_heap->GetCPUDescriptorHandleForHeapStart());
    debugf(L"heap start: {}\n", desc_h.ptr);
    // auto woodCrateTex = mTextures["woodCrateTex"]->Resource;

    auto create_srv = [&desc_h, this](ID3D12Resource* tex) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Format = tex->GetDesc().Format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = tex->GetDesc().MipLevels;
        srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;

        desc_h.Offset(1, m_cbv_srv_uav_desc_size);
        debugf(L" handle after offset: {}\n", desc_h.ptr);

        m_device->CreateShaderResourceView(tex, &srv_desc, desc_h);
    };
    if (use_texture_from_file) {
        create_srv(m_texture1.get());
    } else {
        create_srv(m_texture2.get());
    }
}

void App::build_constant_buffers() {
    m_object_cb = std::make_unique<d3d_util::UploadBuffer<ObjectConstants>>(m_device.get(), 1, true);

    uint32_t cb_byte_size = d3d_util::calc_constant_buffer_byte_size(sizeof(ObjectConstants));

    D3D12_GPU_VIRTUAL_ADDRESS cbAddress = m_object_cb->resource()->GetGPUVirtualAddress();
    // Offset to the ith object constant buffer in the buffer.
    int boxCBufIndex = 0;
    cbAddress += boxCBufIndex * cb_byte_size;

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
    cbvDesc.BufferLocation = cbAddress;
    cbvDesc.SizeInBytes = d3d_util::calc_constant_buffer_byte_size(sizeof(ObjectConstants));

    m_device->CreateConstantBufferView(&cbvDesc, m_cbv_heap->GetCPUDescriptorHandleForHeapStart());
}

void App::build_root_signature() {
    // First root param is a "table", with a CBV and then an SRV descriptor.
    // CD3DX12_DESCRIPTOR_RANGE texTable;
    // texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE); // DATA_STATIC);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE); // DATA_STATIC);

    CD3DX12_ROOT_PARAMETER1 params[2];

    // Perfomance TIP: Order from most frequent to least frequent.
    params[0].InitAsDescriptorTable(1, ranges, D3D12_SHADER_VISIBILITY_ALL);
    params[1].InitAsDescriptorTable(1, ranges+1, D3D12_SHADER_VISIBILITY_PIXEL);
    // slotRootParameter[1].InitAsConstantBufferView(1); // <- "register" number. Used 0 in table above.
    // slotRootParameter[2].InitAsConstantBufferView(2);
    // slotRootParameter[3].InitAsConstantBufferView(3);

    auto samplers = get_static_samplers();

    D3D12_FEATURE_DATA_ROOT_SIGNATURE feature_data = {};

    // This is the highest version the sample supports. If CheckFeatureSupport succeeds, the HighestVersion returned
    // will not be greater than this.
    /*feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &feature_data, sizeof(feature_data)))) {
        feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }*/

    // A root signature is an array of root parameters.
    // CD3DX12_ROOT_SIGNATURE_DESC rs_desc(4, slotRootParameter, (UINT)samplers.size(), samplers.data(),
    //                                         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc;
    rs_desc.Init_1_1(2, params, (UINT)samplers.size(), samplers.data(),
                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    // create a root signature with a single slot which points to a descriptor range consisting of a single constant
    // buffer
    com_ptr<ID3DBlob> serialized = nullptr;
    com_ptr<ID3DBlob> errorBlob = nullptr;
    // HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
    //                                          serialized.put(), errorBlob.put());
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1_1, // feature_data.HighestVersion,
                                                       serialized.put(), errorBlob.put());
    if (errorBlob != nullptr) {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    check_hresult(hr);

    check_hresult(m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                serialized->GetBufferSize(), __uuidof(m_root_signature),
                                                m_root_signature.put_void()));
    m_root_signature->SetName(L"m_root_sig");
}

void App::build_shaders_and_input_layout()
{
    HRESULT hr = S_OK;
    m_shader_byte_code.vs = d3d_util::compile_shader(L"shaders.hlsl", nullptr, "vert_shader", "vs_5_0");
    m_shader_byte_code.ps = d3d_util::compile_shader(L"shaders.hlsl", nullptr, "pix_shader", "ps_5_0");

    m_input_layout = {
        // {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        // {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };
}

void App::make_geo() {
    // For a square 
    // 0  1
    // 3  2
    std::array<Vertex, 4> vertices = {
        Vertex({ XMFLOAT2(-0.7f,  0.7f), XMFLOAT2(0,0) }),
        Vertex({ XMFLOAT2( 0.7f,  0.8f), XMFLOAT2(1,0) }),
        Vertex({ XMFLOAT2( 0.6f, -0.7f), XMFLOAT2(1,1) }),
        Vertex({ XMFLOAT2(-0.7f, -0.7f), XMFLOAT2(0,1) })
    };

    std::array<std::uint16_t, 6> indices = { 0,1,3,  1,2,3 };

    const uint32_t vbByteSize = vertices.size() * sizeof(Vertex);
    const uint32_t ibByteSize = (uint32_t)indices.size() * sizeof(std::uint16_t);

    m_geo = std::make_unique<MeshGeometry>();
    m_geo->name = "square_geo";

    check_hresult(D3DCreateBlob(vbByteSize, m_geo->vbuf_cpu.put()));
    memcpy(m_geo->vbuf_cpu->GetBufferPointer(), vertices.data(), vbByteSize);

    check_hresult(D3DCreateBlob(ibByteSize, m_geo->ibuf_cpu.put()));
    memcpy(m_geo->ibuf_cpu->GetBufferPointer(), indices.data(), ibByteSize);

    m_geo->vbuf_gpu = d3d_util::create_default_buffer(m_device.get(),
        m_command_list.get(), vertices.data(), vbByteSize, m_geo->vbuf_uploader);

    m_geo->ibuf_gpu = d3d_util::create_default_buffer(m_device.get(),
        m_command_list.get(), indices.data(), ibByteSize, m_geo->ibuf_uploader);

    m_geo->vertex_stride = sizeof(Vertex);
    m_geo->vbuf_size = vbByteSize;
    m_geo->index_format = DXGI_FORMAT_R16_UINT;
    m_geo->ibuf_size = ibByteSize;
    m_geo->index_count = indices.size();

    SubmeshGeometry submesh;
    submesh.index_count = (uint32_t)indices.size();
    submesh.start_index = 0;
    submesh.base_vertex = 0;

    m_geo->parts["square"] = submesh;

}

void App::build_pso() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
    ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_back_buffer_format;
    psoDesc.SampleDesc.Count = m_4x_msaa_state ? 4 : 1;
    psoDesc.SampleDesc.Quality = m_4x_msaa_state ? (m_4x_msaa_quality - 1) : 0;
    psoDesc.DSVFormat = m_depth_stencil_format;

    psoDesc.pRootSignature = m_root_signature.get(); // Added a texture to this one, not using #2 ?
    psoDesc.VS = { reinterpret_cast<BYTE*>(m_shader_byte_code.vs->GetBufferPointer()),
                  m_shader_byte_code.vs->GetBufferSize() };
    psoDesc.PS = { reinterpret_cast<BYTE*>(m_shader_byte_code.ps->GetBufferPointer()),
                  m_shader_byte_code.ps->GetBufferSize() };
    psoDesc.InputLayout = { m_input_layout.data(), (UINT)m_input_layout.size() };
    check_hresult(m_device->CreateGraphicsPipelineState(&psoDesc, __uuidof(m_pso), m_pso.put_void()));
}


std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> App::get_static_samplers() {
    // Applications usually only need a handful of samplers.  So just define them all up front
    // and keep them available as part of the root signature.

    const CD3DX12_STATIC_SAMPLER_DESC pointWrap(0,                                // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT,   // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(1,                                 // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT,    // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(2,                                // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,  // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(3,                                 // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,   // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(4,                               // shaderRegister
        D3D12_FILTER_ANISOTROPIC,        // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressW
        0.0f,                            // mipLODBias
        8);                              // maxAnisotropy

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(5,                                // shaderRegister
        D3D12_FILTER_ANISOTROPIC,         // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressW
        0.0f,                             // mipLODBias
        8);                               // maxAnisotropy

    return { pointWrap, pointClamp, linearWrap, linearClamp, anisotropicWrap, anisotropicClamp };
}

// Try to create a GPU texture and draw on it with D2D, so I can use it as a shader resource.
com_ptr<ID3D12Resource> App::draw_on_texture() {
    HRESULT hr;

    // 1. Create a D11 device. Apparently you need that for D2D.
    //    Create an 11 device wrapped around the 12 device and share 12's command queue.
    UINT d3d11_device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    d3d11_device_flags |= D3D11_CREATE_DEVICE_DEBUG;
    // com_ptr<ID3D11Device> d3d11_device; // <-- was this getting de-allocated ?
    hr = D3D11On12CreateDevice(m_device.get(), d3d11_device_flags, nullptr, 0,
                               reinterpret_cast<IUnknown**>(&m_command_queue), 1, 0,
                               m_d3d11_device.put(), /* device ctx: */ nullptr, nullptr);
    check_hresult(hr);
    // Query the 11On12 device from the 11 device.
    hr = m_d3d11_device.as(__uuidof(m_11on12_device), m_11on12_device.put_void());
    check_hresult(hr);

    // 2. Create the factories for Direct 2D and DirectWrite
    // - D2D one already created above. 

    // com_ptr<IDXGIDevice> dxgi_device;
    // check_hresult(m_11on12_device.as(__uuidof(IDXGIDevice), dxgi_device.put_void())); // was messed up cast? 
    // check_hresult(m_d2d_factory->CreateDevice(dxgi_device.get(), m_d2d_device.put()));
    // D2D1_DEVICE_CONTEXT_OPTIONS device_options = D2D1_DEVICE_CONTEXT_OPTIONS_NONE;
    // check_hresult(m_d2d_device->CreateDeviceContext(device_options, m_d2d_device_context.put()));

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), 
                             // Why doesn't this work without cast? Base** is not Sub** ?
                             reinterpret_cast<IUnknown**>(m_dwrite_factory.put()));
    check_hresult(hr);

    // 3. Create texture

    // Describe and create a Texture2D.
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.Width = 256;
    textureDesc.Height = 256;
    textureDesc.Flags = /*D3D12_RESOURCE_FLAG_NONE;*/
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    com_ptr<ID3D12Resource> texture;
    CD3DX12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE_DEFAULT);
    // CD3DX12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE_);
    hr = m_device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(texture),
                                           texture.put_void());
    check_hresult(hr);
    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture.get(), 0, 1);

    // Is that what to do? Create a wrapped resource for the texture ?
        // D3D11_RESOURCE_FLAGS d3d11Flags = {D3D11_BIND_SHADER_RESOURCE};  // ??
        // D3D11_RESOURCE_FLAGS d3d11Flags = {D3D11_BIND_RENDER_TARGET};  // ??
    D3D11_RESOURCE_FLAGS d3d11_flags = { D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE };  // ??
    hr = m_11on12_device->CreateWrappedResource(texture.get(), &d3d11_flags,
                                                // D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT,
                                                __uuidof(m_d11_texture), m_d11_texture.put_void());
    check_hresult(hr);

    IDXGISurface* surface;
    hr = m_d11_texture.as(__uuidof(IDXGISurface), reinterpret_cast<void**>(&surface)); 
    check_hresult(hr);

    // m_d2d_factory->GetDesktopDpi(&dpiX, &dpiY); // deprecated
    float dpi = GetDpiForWindow(m_main_window_h);
    debugf(L"GetDpiForWindow => {}\n", dpi);
    D2D1_RENDER_TARGET_PROPERTIES props;
    props.dpiX = dpi;
    props.dpiY = dpi;
    props.type = D2D1_RENDER_TARGET_TYPE_HARDWARE;
    props.pixelFormat = D2D1_PIXEL_FORMAT(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    // props.pixelFormat = D2D1_PIXEL_FORMAT(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED);
    props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    hr = m_d2d_factory->CreateDxgiSurfaceRenderTarget(surface, props, m_render_target.put());
    check_hresult(hr);

    // 4. Create a brush
    // m_render_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &m_black_brush);
    m_render_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), m_red_brush.put());

    // 5. Draw something

    ID2D1RenderTarget* rt = m_render_target.get();
    auto tex = m_d11_texture.get();
    m_11on12_device->AcquireWrappedResources(&tex, 1);
    rt->BeginDraw();
    rt->DrawRectangle({10, 10, 100, 100}, m_red_brush.get());
    rt->EndDraw();
    // rt->Flush();
    m_11on12_device->ReleaseWrappedResources(&tex, 1);


    return texture;
}


#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
