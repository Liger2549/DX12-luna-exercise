
#include "BoxApp.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

constexpr UINT CBV_SRV_UAV_HEAP_CAPACITY = 16384;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        BoxApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

BoxApp::BoxApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{

}

BoxApp::~BoxApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();
}

bool BoxApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    // We will upload on the direct queue for the book samples, but 
    // copy queue would be better for real game.
    mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

    BuildBoxGeometry(md3dDevice.Get(), *mUploadBatch.get());

    // Kick off upload work asyncronously.
    std::future<void> result = mUploadBatch->End(mCommandQueue.Get());

    // Other init work...
    BuildCbvSrvUavDescriptorHeap();
    BuildConstantBuffers();
    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildPSO();

    // Block until the upload work is complete.
    result.wait();

    return true;
}

void BoxApp::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
}

void BoxApp::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void BoxApp::Update(const GameTimer& gt)
{
    // Convert Spherical to Cartesian coordinates.
    float x = mRadius * sinf(mPhi) * cosf(mTheta);
    float z = mRadius * sinf(mPhi) * sinf(mTheta);
    float y = mRadius * cosf(mPhi);

    // Build the view matrix.
    XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
    XMStoreFloat4x4(&mView, view);

    XMMATRIX world = XMLoadFloat4x4(&mWorld);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);
    XMMATRIX viewProj = view * proj;

    // Update the per-object buffer with the latest world matrix.
    for (int i = 0; i < 3; i++)
    {
		for (int j = 0; j < 3; j++)
		{
			int index = i * 3 + j;
			ObjectConstants boxConstants;
			XMMATRIX boxWorld = XMMatrixTranslation(-5.0f + j * 5.0f, 0.0f, -5.0f + i * 5.0f);
			XMStoreFloat4x4(&boxConstants.World, XMMatrixTranspose(boxWorld));
			mObjectCB->CopyData(index, boxConstants);
		}
    }

    // Update the per-pass buffer with the latest viewProj matrix.
    PassConstants passConstants;
    XMStoreFloat4x4(&passConstants.ViewProj, XMMatrixTranspose(viewProj));
    mPassCB->CopyData(0, passConstants);
}

void BoxApp::Draw(const GameTimer& gt)
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();

    UpdateImgui(gt);

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(mDirectCmdListAlloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mSolidPSO.Get()));

    ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    // Indicate a state transition on the resource usage.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the back buffer and depth buffer.
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    mCommandList->SetPipelineState(mDrawWireframe ? mWireframePSO.Get() : mSolidPSO.Get());
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
	
    mCommandList->SetGraphicsRootDescriptorTable(
        ROOT_ARG_PASS_CBV,
        cbvSrvUavHeap.GpuHandle(mPassCBHeapIndex));

    mCommandList->IASetVertexBuffers(0, 1, &mGeo->VertexBufferView());
    mCommandList->IASetIndexBuffer(&mGeo->IndexBufferView());
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // bind box CBV
	for (int i = 0; i < 9; i++)
	{
		mCommandList->SetGraphicsRootDescriptorTable(
			ROOT_ARG_OBJECT_CBV,
			cbvSrvUavHeap.GpuHandle(mBoxCBHeapIndex[i]));
        // draw the box geometry
        mCommandList->DrawIndexedInstanced(
            mGeo->DrawArgs["Box"].IndexCount,
            1,  // instanceCount
            mGeo->DrawArgs["Box"].StartIndexLocation,
            mGeo->DrawArgs["Box"].BaseVertexLocation,
            0); // startInstanceLocation

	}	

    // Draw imgui UI.
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

    // Indicate a state transition on the resource usage.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    // Done recording commands.
    ThrowIfFailed(mCommandList->Close());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Swap the back and front buffers
    DXGI_PRESENT_PARAMETERS presentParams = { 0 };
    ThrowIfFailed(mSwapChain->Present1(0, 0, &presentParams));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    // Wait until frame commands are complete.  This waiting is inefficient and is
    // done for simplicity.  Later we will show how to organize our rendering code
    // so we do not have to wait per frame.
    FlushCommandQueue();
}

void BoxApp::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    //
    // Define a panel to render GUI elements.
    // 
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    ImGui::Checkbox("Wireframe", &mDrawWireframe);

    GraphicsMemoryStatistics gfxMemStats = GraphicsMemory::Get(md3dDevice.Get()).GetStatistics();

    if (ImGui::CollapsingHeader("VideoMemoryInfo"))
    {
        static float vidMemPollTime = 0.0f;
        vidMemPollTime += gt.DeltaTime();

        static DXGI_QUERY_VIDEO_MEMORY_INFO videoMemInfo;
        if (vidMemPollTime >= 1.0f) // poll every second
        {
            mDefaultAdapter->QueryVideoMemoryInfo(
                0, // assume single GPU
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL, // interested in local GPU memory, not shared
                &videoMemInfo);

            vidMemPollTime -= 1.0f;
        }

        ImGui::Text("Budget (bytes): %u", videoMemInfo.Budget);
        ImGui::Text("CurrentUsage (bytes): %u", videoMemInfo.CurrentUsage);
        ImGui::Text("AvailableForReservation (bytes): %u", videoMemInfo.AvailableForReservation);
        ImGui::Text("CurrentReservation (bytes): %u", videoMemInfo.CurrentReservation);

    }
    if (ImGui::CollapsingHeader("GraphicsMemoryStatistics"))
    {
        ImGui::Text("Bytes of memory in-flight: %u", gfxMemStats.committedMemory);
        ImGui::Text("Total bytes used: %u", gfxMemStats.totalMemory);
        ImGui::Text("Total page count: %u", gfxMemStats.totalPages);
    }

    ImGui::End();

    ImGui::Render();
}

void BoxApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    if (!ImGui::GetCurrentContext()) return;

    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void BoxApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    if (!ImGui::GetCurrentContext()) return;

    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void BoxApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if (!ImGui::GetCurrentContext()) return;

    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        if ((btnState & MK_LBUTTON) != 0)
        {
            // Make each pixel correspond to a quarter of a degree.
            float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
            float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

            // Update angles based on input to orbit camera around box.
            mTheta += dx;
            mPhi += dy;

            // Restrict the angle mPhi.
            mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
        }
        else if ((btnState & MK_RBUTTON) != 0)
        {
            // Make each pixel correspond to 0.005 unit in the scene.
            float dx = 0.005f * static_cast<float>(x - mLastMousePos.x);
            float dy = 0.005f * static_cast<float>(y - mLastMousePos.y);

            // Update the camera radius based on input.
            mRadius += dx - dy;

            // Restrict the radius.
            mRadius = MathHelper::Clamp(mRadius, 3.0f, 15.0f);
        }

        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }
}

void BoxApp::BuildCbvSrvUavDescriptorHeap()
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

    InitImgui(cbvSrvUavHeap);
}

void BoxApp::BuildConstantBuffers()
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();

    for (int i = 0; i < 9; i++)
    {
        mBoxCBHeapIndex[i] = cbvSrvUavHeap.NextFreeIndex();

    }
    mPassCBHeapIndex = cbvSrvUavHeap.NextFreeIndex();

    const UINT elementCount = 9;
    const bool isConstantBuffer = true;

    mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(
        md3dDevice.Get(),
        elementCount,
        isConstantBuffer);

    mPassCB = std::make_unique<UploadBuffer<PassConstants>>(
        md3dDevice.Get(),
        1,
        isConstantBuffer);

    // Constant buffers must be a multiple of the
    // minimum hardware allocation size (usually 256 bytes).
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

    // Box CBV
	std::vector<D3D12_GPU_VIRTUAL_ADDRESS> boxCBAddress(elementCount);
	std::vector<D3D12_CONSTANT_BUFFER_VIEW_DESC> cbvBoxDesc(elementCount);
    for (int i = 0; i < elementCount; i++)
    {
        boxCBAddress[i] =
            mObjectCB->Resource()->GetGPUVirtualAddress() +
            i * objCBByteSize;
        cbvBoxDesc[i].BufferLocation = boxCBAddress[i];
        cbvBoxDesc[i].SizeInBytes = objCBByteSize;
        md3dDevice->CreateConstantBufferView(
            &cbvBoxDesc[i],
            cbvSrvUavHeap.CpuHandle(mBoxCBHeapIndex[i]));
    }

	// Pass CBV - element 0
    D3D12_GPU_VIRTUAL_ADDRESS passCBAddress =
        mPassCB->Resource()->GetGPUVirtualAddress();
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvPassDesc;
    cbvPassDesc.BufferLocation = passCBAddress;
    cbvPassDesc.SizeInBytes = passCBByteSize;
    md3dDevice->CreateConstantBufferView(
        &cbvPassDesc,
        cbvSrvUavHeap.CpuHandle(mPassCBHeapIndex));
}

void BoxApp::BuildRootSignature()
{
    // Shader programs typically require resources as input (constant buffers,
    // textures, samplers).  The root signature defines the resources the shader
    // programs expect.  If we think of the shader programs as a function, and
    // the input resources as function parameters, then the root signature can be
    // thought of as defining the function signature.  

    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER slotRootParameter[ROOT_ARG_COUNT] = {};

    // Create a table for per-object constants. Arguments would need to be
    // set once per object.
    CD3DX12_DESCRIPTOR_RANGE objectCbvTable;
    UINT numDescriptors = 1;
    UINT baseRegister = 0;
    objectCbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
        numDescriptors, baseRegister);

    // Create a table for per-pass constants. Arguments would need to be
    // set once per pass.
    CD3DX12_DESCRIPTOR_RANGE passCbvTable;
    baseRegister = 1;
    passCbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
        numDescriptors, baseRegister);

    slotRootParameter[ROOT_ARG_OBJECT_CBV].InitAsDescriptorTable(1, &objectCbvTable);
    slotRootParameter[ROOT_ARG_PASS_CBV].InitAsDescriptorTable(1, &passCbvTable);

    // A root signature is an array of root parameters.
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        ROOT_ARG_COUNT,
        slotRootParameter,
        0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    // create a root signature
    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(),
        errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));
}

void BoxApp::BuildShadersAndInputLayout()
{
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS
#else
#define COMMA_DEBUG_ARGS
#endif

    std::vector<LPCWSTR> vsArgs = std::vector<LPCWSTR>{ L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> psArgs = std::vector<LPCWSTR>{ L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

    mvsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", vsArgs);
    mpsByteCode = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", psArgs);

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

}

void BoxApp::BuildBoxGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch)
{
    // Box
    Vertex vb1 = { XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT4(Colors::White) };
	Vertex vb2 = { XMFLOAT3(-1.0f, +1.0f, -1.0f), XMFLOAT4(Colors::Black) };
	Vertex vb3 = { XMFLOAT3(+1.0f, +1.0f, -1.0f), XMFLOAT4(Colors::Red) };
	Vertex vb4 = { XMFLOAT3(+1.0f, -1.0f, -1.0f), XMFLOAT4(Colors::Green) };
	Vertex vb5 = { XMFLOAT3(-1.0f, -1.0f, +1.0f), XMFLOAT4(Colors::Blue) };
	Vertex vb6 = { XMFLOAT3(-1.0f, +1.0f, +1.0f), XMFLOAT4(Colors::Yellow) };
	Vertex vb7 = { XMFLOAT3(+1.0f, +1.0f, +1.0f), XMFLOAT4(Colors::Cyan) };
	Vertex vb8 = { XMFLOAT3(+1.0f, -1.0f, +1.0f), XMFLOAT4(Colors::Magenta) };
	std::array<Vertex, 8> vertices =
	{
		vb1, vb2, vb3, vb4, vb5, vb6, vb7, vb8
	};

	std::array<std::uint16_t, 36> indices = {
		// Front face
		0, 1, 2,
		0, 2, 3,

		// Back face
		4, 6, 5,
		4, 7, 6,
		
        // Left face
		4, 5, 1,
		4, 1, 0,
		
        // Right face
		3, 2, 6,
		3, 6, 7,
		
        // Top face
		1, 5, 6,
		1, 6, 2,
		
        // Bottom face
		4, 0, 3,
		4, 3, 7
	};

    // Box geo
    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

    mGeo = std::make_unique<MeshGeometry>();
    mGeo->Name = "BoxGeo";

    mGeo->VertexBufferCPU.resize(vbByteSize);
    CopyMemory(mGeo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

    CreateStaticBuffer(
        device, uploadBatch,
        vertices.data(), (UINT)vertices.size(), sizeof(Vertex),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        &mGeo->VertexBufferGPU);
    mGeo->VertexByteStride = sizeof(Vertex);
    mGeo->VertexBufferByteSize = vbByteSize;

    // Box indices
    mGeo->IndexBufferCPU.resize(ibByteSize);
    CopyMemory(mGeo->IndexBufferCPU.data(), indices.data(), ibByteSize);
    CreateStaticBuffer(
        device, uploadBatch,
        indices.data(), (UINT)indices.size(), sizeof(std::uint16_t),
        D3D12_RESOURCE_STATE_INDEX_BUFFER,
        &mGeo->IndexBufferGPU);
    mGeo->IndexFormat = DXGI_FORMAT_R16_UINT;
    mGeo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;
	submesh.VertexCount = (UINT)vertices.size();

    // Box that tightly contains all the geometry. This 
    // is used in later chapters of the book.
    submesh.Bounds = BoundingBox(
        XMFLOAT3(0.0f, 0.0f, 0.0f),  // center
        XMFLOAT3(1.0f, 1.0f, 1.0f)); // extents

    mGeo->DrawArgs["Box"] = submesh;
}

void BoxApp::BuildPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc = d3dUtil::InitDefaultPso(
        mBackBufferFormat,
        mDepthStencilFormat,
        mInputLayout,
        mRootSignature.Get(),
        mvsByteCode.Get(), mpsByteCode.Get());

	basePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // so we can see the back faces of the box in solid mode
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &basePsoDesc,
        IID_PPV_ARGS(&mSolidPSO)));

    // Create a new PSO based off the default PSO:
    D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframePsoDesc = basePsoDesc;
    wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	//wireframePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // so we can see the back faces of the wireframe box
	wireframePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT; // cull front faces so we can see the back faces of the wireframe box

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &wireframePsoDesc,
        IID_PPV_ARGS(&mWireframePSO)));
}