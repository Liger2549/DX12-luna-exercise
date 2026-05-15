#pragma once

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/MeshGen.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Common/Camera.h"

struct VPosData
{
    DirectX::XMFLOAT3 Pos;
};

struct VColorData
{
    DirectX::XMFLOAT4 Color;
};

struct ObjectConstants
{
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
};

struct PassConstants
{
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
};

enum ROOT_ARG
{
    ROOT_ARG_OBJECT_CBV = 0,
    ROOT_ARG_PASS_CBV,
    ROOT_ARG_COUNT
};

// Second Vertex buffer for color data
struct MeshGeometryEx : public MeshGeometry
{
    std::vector<byte> VertexBufferCPU2;
    Microsoft::WRL::ComPtr<ID3D12Resource> VertexBufferGPU2 = nullptr;

    UINT VertexByteStirde2 = 0;
	UINT VertexBufferByteSize2 = 0;

	D3D12_VERTEX_BUFFER_VIEW VertexBufferView2()const
	{
		D3D12_VERTEX_BUFFER_VIEW vbv;
		vbv.BufferLocation = VertexBufferGPU2->GetGPUVirtualAddress();
		vbv.StrideInBytes = VertexByteStirde2;
		vbv.SizeInBytes = VertexBufferByteSize2;
		return vbv;
	}
};

class BoxApp : public D3DApp
{
public:
    BoxApp(HINSTANCE hInstance);
    BoxApp(const BoxApp& rhs) = delete;
    BoxApp& operator=(const BoxApp& rhs) = delete;
    ~BoxApp();

    virtual bool Initialize()override;

private:
    virtual void CreateRtvAndDsvDescriptorHeaps()override;
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void UpdateImgui(const GameTimer& gt)override;
    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void BuildCbvSrvUavDescriptorHeap();
    void BuildConstantBuffers();
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildBoxGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch);
    void BuildPSO();

private:

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

    uint32_t mBoxCBHeapIndex = -1;
    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;

    uint32_t mPassCBHeapIndex = -1;
    std::unique_ptr<UploadBuffer<PassConstants>> mPassCB = nullptr;

	// We are going to use two vertex buffers, one for position and one for color, 
    // so we need to create a new struct that extends MeshGeometry to hold the second vertex buffer.
    std::unique_ptr<MeshGeometryEx> mBoxGeo = nullptr;

    Microsoft::WRL::ComPtr<IDxcBlob> mvsByteCode = nullptr;
    Microsoft::WRL::ComPtr<IDxcBlob> mpsByteCode = nullptr;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> mSolidPSO = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mWireframePSO = nullptr;

    DirectX::XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.25f * DirectX::XM_PI;
    float mPhi = 0.25f * DirectX::XM_PI;
    float mRadius = 5.0f;

    POINT mLastMousePos;

    bool mDrawWireframe = false;
};