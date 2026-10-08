/**
 * Copyright (C) 2015 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/d3d8to9#license
 */

#include "d3dx9.hpp"
#include "d3d8to9.hpp"
#include "fakefull.hpp"
#include "msaa.hpp"
#include <regex>
#include <assert.h>
#include "u2shaders.hpp"
#include "perf.hpp"
static U2Perf Perf;   // frame times at Present -> U2Shaders.log (perf.hpp)

static U2Shaders U2;
// blood.hpp logs through the layer's log, and the mod's native DLL reaches it by this export
static void U2BloodLog(const char *S) { U2.Message("%s", S); }
static int U2BloodLogSet = (U2Blood::Log = U2BloodLog, U2Runs::Log = U2BloodLog, U2Streaks::Log = U2BloodLog, U2Strings::Log = U2BloodLog, U2Lens::Log = U2BloodLog, 0);
// the mod's blood commands: floor pools (blood.hpp), wall runs (runs.hpp: run, drip, rstop) and
// streaks down characters (streaks.hpp: streak, streaks, streakclear) and goo strings between body
// parts (strings.hpp: string, stringoff, stringclear); blood on the player's hands and weapon
// (streaks.hpp: hand, hands, handclear) and drops on the lens (lens.hpp: lens, lensat, lensclear)
extern "C" __declspec(dllexport) int __cdecl U2BloodCommand(const char *Cmd)
{
	char Word[16] = "";
	sscanf_s(Cmd, "%15s", Word, (unsigned)sizeof(Word));
	if (U2Streaks::Handles(Word))
		return U2Streaks::Command(Cmd);
	if (U2Strings::Handles(Word))
		return U2Strings::Command(Cmd);
	if (U2Lens::Handles(Word))
		return U2Lens::Command(Cmd);
	return U2Runs::Handles(Word) ? U2Runs::Command(Cmd) : U2Blood::Command(Cmd);
}
// a shotp from outside (AdventNative's "Capture"): the next presented frame saved as System\ShotP#####.bmp;
// Mask 1 also saves the character mask beside it (shotmask), 0 not, -1 as U2Shaders.ini says
extern "C" __declspec(dllexport) int __cdecl U2ShotP(int Mask) { if (Mask >= 0) U2.ShotMask = Mask != 0; U2.ShotPWant = true; return 1; }
bool U2WantsBuffers()
{
	if (!U2.Loaded)
		U2.Load();
	return U2.Capture || U2.LightProbeFrames > 0;
}

struct VertexShaderInfo
{
	IDirect3DVertexShader9 *Shader = nullptr;
	IDirect3DVertexDeclaration9 *Declaration = nullptr;
};

Direct3DDevice8::Direct3DDevice8(Direct3D8 *d3d, IDirect3DDevice9 *ProxyInterface, DWORD BehaviorFlags, D3DFORMAT ZBufferFormat, BOOL EnableZBufferDiscarding) :
	D3D(d3d), ProxyInterface(ProxyInterface), ZBufferDiscarding(EnableZBufferDiscarding)
{
	ProxyAddressLookupTable = new AddressLookupTable(this);

	const HDC hDC = GetDC(nullptr);
	IsPaletteSupported = (::GetDeviceCaps(hDC, RASTERCAPS) & RC_PALETTE) != 0;
	ReleaseDC(nullptr, hDC);

	IsMixedVertexProcessingDevice = (BehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) != 0;

	CurrentZBufferBitCount = GetDepthStencilBitCount(ZBufferFormat);

	// The default value of D3DRS_POINTSIZE_MIN is 0.0f in D3D8,
	// whereas in D3D9 it is 1.0f, so adjust it as needed
	ProxyInterface->SetRenderState(D3DRS_POINTSIZE_MIN, (DWORD)0.0f);
	// The DEPTHBIAS value of -0.0f works differently than 0.0f
	// Some games require defaulting to -0.0f to work correctly
	const float DepthBias = -0.0f;
	ProxyInterface->SetRenderState(D3DRS_DEPTHBIAS, *(const DWORD *)&DepthBias);
}
Direct3DDevice8::~Direct3DDevice8()
{
	U2.OnDestroy();
	delete ProxyAddressLookupTable;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (riid == __uuidof(IDirect3DDevice8) ||
		riid == __uuidof(IUnknown))
	{
		AddRef();
		*ppvObj = static_cast<IDirect3DDevice8 *>(this);

		return S_OK;
	}

	const HRESULT hr = ProxyInterface->QueryInterface(ConvertREFIID(riid), ppvObj);
	if (SUCCEEDED(hr))
		GenericQueryInterface(riid, ppvObj, this);

	return hr;
}
ULONG STDMETHODCALLTYPE Direct3DDevice8::AddRef()
{
	U2GameRefs++;
	ULONG LastRefCount = ProxyInterface->AddRef();

	// Shaders and state blocks increase ref counter in d3d9 but not in d3d8
	DWORD ExtraRefs = VertexShaderAndDeclarationCount + PixelShaderHandles.size() + StateBlockTokens.size();
	if (ExtraRefs <= LastRefCount)
	{
		LastRefCount = LastRefCount - ExtraRefs;
	}

	return LastRefCount;
}

ULONG STDMETHODCALLTYPE Direct3DDevice8::Release()
{
	// the game lets go of the device: U2Shaders' own textures, targets and shaders each hold a
	// reference to the d3d9 device, so release them first, or the device never reaches zero, is
	// never destroyed, and every fullscreen/windowed switch leaks one (the game then loops making
	// new devices until Direct3D refuses) while the old objects get bound to the new device
	if (U2GameRefs > 0 && --U2GameRefs == 0)
		U2.OnDestroy();

	// Get current value before releasing the device reference
	ULONG LastRefCount = ProxyInterface->AddRef();
	LastRefCount = ProxyInterface->Release();

	// Shaders and StateBlocks are destroyed alongside the device that created them in D3D8 but not in D3D9
	// so we need to Release any remaining shaders or state blocks when the device is released to mirror that behaviour
	DWORD ExtraRefs = VertexShaderAndDeclarationCount + PixelShaderHandles.size() + StateBlockTokens.size();
	if (ExtraRefs <= LastRefCount)
	{
		LastRefCount = LastRefCount - ExtraRefs;
		if (LastRefCount == 1)
		{
			// Release shaders and state blocks when only one reference is left
			ReleaseShadersAndStateBlocks();
		}
	}

	// Release device reference
	LastRefCount = ProxyInterface->Release();

	if (LastRefCount == 0)
		delete this;

	return LastRefCount;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::TestCooperativeLevel()
{
	return ProxyInterface->TestCooperativeLevel();
}
UINT STDMETHODCALLTYPE Direct3DDevice8::GetAvailableTextureMem()
{
	return ProxyInterface->GetAvailableTextureMem();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::ResourceManagerDiscardBytes(DWORD Bytes)
{
	UNREFERENCED_PARAMETER(Bytes);

	return ProxyInterface->EvictManagedResources();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDirect3D(IDirect3D8 **ppD3D8)
{
	if (ppD3D8 == nullptr)
		return D3DERR_INVALIDCALL;

	D3D->AddRef();
	*ppD3D8 = D3D;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDeviceCaps(D3DCAPS8 *pCaps)
{
	if (pCaps == nullptr)
		return D3DERR_INVALIDCALL;

	D3DCAPS9 DeviceCaps;

	const HRESULT hr = ProxyInterface->GetDeviceCaps(&DeviceCaps);
	if (FAILED(hr))
		return hr;

	ConvertCaps(DeviceCaps, *pCaps);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDisplayMode(D3DDISPLAYMODE *pMode)
{
	return ProxyInterface->GetDisplayMode(0, pMode);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
{
	return ProxyInterface->GetCreationParameters(pParameters);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap)
{
	if (pCursorBitmap == nullptr)
		return D3DERR_INVALIDCALL;

	auto pCursorBitmapImpl = static_cast<Direct3DSurface8 *>(pCursorBitmap);
	return ProxyInterface->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmapImpl->GetProxyInterface());
}
void STDMETHODCALLTYPE Direct3DDevice8::SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags)
{
	ProxyInterface->SetCursorPosition(XScreenSpace, YScreenSpace, Flags);
}
BOOL STDMETHODCALLTYPE Direct3DDevice8::ShowCursor(BOOL bShow)
{
	return ProxyInterface->ShowCursor(bShow);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8 *pPresentationParameters, IDirect3DSwapChain8 **ppSwapChain)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateAdditionalSwapChain" << "(" << this << ", " << pPresentationParameters << ", " << ppSwapChain << ")' ..." << std::endl;
#endif

	if (pPresentationParameters == nullptr || ppSwapChain == nullptr)
		return D3DERR_INVALIDCALL;

	*ppSwapChain = nullptr;

	D3DPRESENT_PARAMETERS PresentParams;
	ConvertPresentParameters(*pPresentationParameters, PresentParams);

	IDirect3DSwapChain9 *SwapChainInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateAdditionalSwapChain(&PresentParams, &SwapChainInterface);
	if (FAILED(hr))
		return hr;

	*ppSwapChain = ProxyAddressLookupTable->FindAddress<Direct3DSwapChain8>(SwapChainInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::Reset(D3DPRESENT_PARAMETERS8 *pPresentationParameters)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::Reset" << "(" << this << ", " << pPresentationParameters << ")' ..." << std::endl;
#endif

	if (pPresentationParameters == nullptr)
		return D3DERR_INVALIDCALL;

	CurrentZBiasRenderState = 0;
	U2.OnLost();
	TexEd().OnLost();                    // texedit: its render targets (bakes, pick target) are DEFAULT pool

	const HRESULT deviceState = ProxyInterface->TestCooperativeLevel();

	if (deviceState == D3DERR_DEVICENOTRESET) {
		while (!StateBlockTokens.empty())
		{
			DWORD Token = *StateBlockTokens.begin();
			DeleteStateBlock(Token);
		}
	}

	D3DPRESENT_PARAMETERS PresentParams;
	ConvertPresentParameters(*pPresentationParameters, PresentParams);
	{
		D3DDEVICE_CREATION_PARAMETERS CP = {};
		ProxyInterface->GetCreationParameters(&CP);
		U2FakeFull::Adjust(PresentParams, CP.hFocusWindow);
		IDirect3D9 *D3D = nullptr;
		if (SUCCEEDED(ProxyInterface->GetDirect3D(&D3D)) && D3D)
		{
			U2Msaa::Adjust(PresentParams, D3D, CP.AdapterOrdinal, CP.DeviceType);
			D3D->Release();
		}
	}

	const HRESULT hr = ProxyInterface->Reset(&PresentParams);

	if (SUCCEEDED(hr))
	{
		// The default value of D3DRS_POINTSIZE_MIN is 0.0f in D3D8,
		// whereas in D3D9 it is 1.0f, so adjust it as needed
		ProxyInterface->SetRenderState(D3DRS_POINTSIZE_MIN, (DWORD) 0.0f);
		// The DEPTHBIAS value of -0.0f works differently than 0.0f
		// Some games require defaulting to -0.0f to work correctly
		float DepthBias = -0.0f;
		ProxyInterface->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&DepthBias);
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion)
{
	UNREFERENCED_PARAMETER(pDirtyRegion);

	U2.MsaaResolve(ProxyInterface);
	if (!U2.MsaaTested && U2Msaa::Wanted() != 0 && U2.Loaded && U2.Frame > 200)
		U2.MsaaSelfTest(ProxyInterface);
	TexEd().OnPresent(U2, ProxyInterface);   // texedit: pick readback, ini/journal watch, adjust bakes
	U2.OnPresent(ProxyInterface);
	Perf.OnPresent([](const char *Line) { U2.Message("%s", Line); });
	U2Crash::Where("the driver's Present");
	const HRESULT Hr = ProxyInterface->Present(pSourceRect, pDestRect, hDestWindowOverride, nullptr);
	U2Crash::Where("the game's own code or draws (between frames)");
	return Hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8 **ppBackBuffer)
{
	if (ppBackBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	*ppBackBuffer = nullptr;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetBackBuffer(0, iBackBuffer, Type, &SurfaceInterface);
	if (FAILED(hr))
		return hr;

	*ppBackBuffer = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus)
{
	return ProxyInterface->GetRasterStatus(0, pRasterStatus);
}
void STDMETHODCALLTYPE Direct3DDevice8::SetGammaRamp(DWORD Flags, const D3DGAMMARAMP *pRamp)
{
	ProxyInterface->SetGammaRamp(0, Flags, pRamp);
}
void STDMETHODCALLTYPE Direct3DDevice8::GetGammaRamp(D3DGAMMARAMP *pRamp)
{
	ProxyInterface->GetGammaRamp(0, pRamp);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
	if (ppTexture == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppTexture = nullptr;

	if (Pool == D3DPOOL_DEFAULT)
	{
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		if ((Usage & D3DUSAGE_DYNAMIC) == 0 &&
			SUCCEEDED(D3D->GetProxyInterface()->CheckDeviceFormat(CreationParams.AdapterOrdinal, CreationParams.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, Format)))
		{
			Usage |= D3DUSAGE_RENDERTARGET;
		}
		else if (Usage != D3DUSAGE_DEPTHSTENCIL)
		{
			Usage |= D3DUSAGE_DYNAMIC;
		}
	}

	IDirect3DTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateTexture(Width, Height, Levels, Usage, Format, Pool, &TextureInterface, nullptr);
	if (FAILED(hr))
		return hr;

	*ppTexture = ProxyAddressLookupTable->FindAddress<Direct3DTexture8>(TextureInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture)
{
	if (ppVolumeTexture == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppVolumeTexture = nullptr;

	IDirect3DVolumeTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, &TextureInterface, nullptr);
	if (FAILED(hr))
		return hr;

	*ppVolumeTexture = ProxyAddressLookupTable->FindAddress<Direct3DVolumeTexture8>(TextureInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture)
{
	if (ppCubeTexture == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppCubeTexture = nullptr;

	IDirect3DCubeTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, &TextureInterface, nullptr);
	if (FAILED(hr))
		return hr;

	*ppCubeTexture = ProxyAddressLookupTable->FindAddress<Direct3DCubeTexture8>(TextureInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
	if (ppVertexBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	*ppVertexBuffer = nullptr;

	IDirect3DVertexBuffer9 *BufferInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateVertexBuffer(Length, Usage, FVF, Pool, &BufferInterface, nullptr);
	if (FAILED(hr))
		return hr;

	*ppVertexBuffer = ProxyAddressLookupTable->FindAddress<Direct3DVertexBuffer8>(BufferInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
	if (ppIndexBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	*ppIndexBuffer = nullptr;

	IDirect3DIndexBuffer9 *BufferInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateIndexBuffer(Length, Usage, Format, Pool, &BufferInterface, nullptr);
	if (FAILED(hr))
		return hr;

	*ppIndexBuffer = ProxyAddressLookupTable->FindAddress<Direct3DIndexBuffer8>(BufferInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface)
{
	if (ppSurface == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppSurface = nullptr;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateRenderTarget(Width, Height, Format, MultiSample, 0, Lockable, &SurfaceInterface, nullptr);
	if (FAILED(hr))
		return hr;

	*ppSurface = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
	if (ppSurface == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppSurface = nullptr;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateDepthStencilSurface(Width, Height, Format, MultiSample, 0, ZBufferDiscarding, &SurfaceInterface, nullptr);
	if (FAILED(hr))
		return hr;

	*ppSurface = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateImageSurface" << "(" << this << ", " << Width << ", " << Height << ", " << Format << ", " << ppSurface << ")' ..." << std::endl;
#endif

	if (ppSurface == nullptr)
		return D3DERR_INVALIDCALL;

	// Only 'CreateImageSurface' clears the content of ppSurface before checking if Format is equal to D3DFMT_UNKNOWN.
	*ppSurface = nullptr;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateOffscreenPlainSurface(Width, Height, Format, D3DPOOL_SYSTEMMEM, &SurfaceInterface, nullptr);

	if (FAILED(hr) && FAILED(ProxyInterface->CreateOffscreenPlainSurface(Width, Height, Format, D3DPOOL_SCRATCH, &SurfaceInterface, nullptr)))
	{
#ifndef D3D8TO9NOLOG
		LOG << "> 'IDirect3DDevice9::CreateOffscreenPlainSurface' failed with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
		return hr;
	}

	*ppSurface = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CopyRects(IDirect3DSurface8 *pSourceSurface, const RECT *pSourceRectsArray, UINT cRects, IDirect3DSurface8 *pDestinationSurface, const POINT *pDestPointsArray)
{
	if (pSourceSurface == nullptr || pDestinationSurface == nullptr || pSourceSurface == pDestinationSurface)
		return D3DERR_INVALIDCALL;

	auto pSourceSurfaceImpl = static_cast<Direct3DSurface8 *>(pSourceSurface);
	auto pDestinationSurfaceImpl = static_cast<Direct3DSurface8 *>(pDestinationSurface);
	U2.MsaaBeforeRead(ProxyInterface, nullptr, pSourceSurfaceImpl->GetProxyInterface());
	U2.OnCopy(pSourceSurfaceImpl->GetProxyInterface(), pDestinationSurfaceImpl->GetProxyInterface());   // shadow maps get copied into their textures

	D3DSURFACE_DESC SourceDesc, DestinationDesc;
	pSourceSurfaceImpl->GetProxyInterface()->GetDesc(&SourceDesc);
	pDestinationSurfaceImpl->GetProxyInterface()->GetDesc(&DestinationDesc);

	if (SourceDesc.Format != DestinationDesc.Format)
		return D3DERR_INVALIDCALL;

	if (GetDepthStencilBitCount(SourceDesc.Format) != 0)
		return D3DERR_INVALIDCALL;

	HRESULT hr = D3DERR_INVALIDCALL;

	// a multisampled source (msaa=N) can't be read as it is: its samples are resolved into a plain
	// target first (a direct read gave one sample per pixel: hard edges)
	IDirect3DSurface9 *Resolved = nullptr;
	IDirect3DSurface9 *Source = pSourceSurfaceImpl->GetProxyInterface();
	if (SourceDesc.MultiSampleType != D3DMULTISAMPLE_NONE
		&& SUCCEEDED(ProxyInterface->CreateRenderTarget(SourceDesc.Width, SourceDesc.Height, SourceDesc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &Resolved, nullptr))
		&& SUCCEEDED(ProxyInterface->StretchRect(Source, nullptr, Resolved, nullptr, D3DTEXF_NONE)))
	{
		Source = Resolved;
		SourceDesc.MultiSampleType = D3DMULTISAMPLE_NONE;
	}

	if (cRects == 0)
		cRects  = 1;

	for (UINT i = 0; i < cRects; i++)
	{
		RECT SourceRect, DestinationRect;

		if (pSourceRectsArray != nullptr)
		{
			SourceRect = pSourceRectsArray[i];
		}
		else
		{
			SourceRect.left = 0;
			SourceRect.right = SourceDesc.Width;
			SourceRect.top = 0;
			SourceRect.bottom = SourceDesc.Height;
		}

		if (pDestPointsArray != nullptr)
		{
			DestinationRect.left = pDestPointsArray[i].x;
			DestinationRect.right = DestinationRect.left + (SourceRect.right - SourceRect.left);
			DestinationRect.top = pDestPointsArray[i].y;
			DestinationRect.bottom = DestinationRect.top + (SourceRect.bottom - SourceRect.top);
		}
		else
		{
			DestinationRect = SourceRect;
		}

		if (SourceDesc.Pool == D3DPOOL_MANAGED || DestinationDesc.Pool != D3DPOOL_DEFAULT)
		{
			hr = D3DERR_INVALIDCALL;
			if (D3DXLoadSurfaceFromSurface != nullptr)
			{
				if (SUCCEEDED(D3DXLoadSurfaceFromSurface(pDestinationSurfaceImpl->GetProxyInterface(), nullptr, &DestinationRect, Source, nullptr, &SourceRect, D3DX_FILTER_NONE, 0)))
				{
					// Explicitly call AddDirtyRect on the surface
					void *pContainer = nullptr;
					if (SUCCEEDED(pDestinationSurfaceImpl->GetContainer(IID_IDirect3DTexture9, &pContainer)) && pContainer)
					{
						IDirect3DTexture9 *pTexture = (IDirect3DTexture9*)pContainer;
						pTexture->AddDirtyRect(&DestinationRect);
						pTexture->Release();
					}
					hr = D3D_OK;
				}
			}
		}
		else if (SourceDesc.Pool == D3DPOOL_DEFAULT)
		{
			hr = ProxyInterface->StretchRect(Source, &SourceRect, pDestinationSurfaceImpl->GetProxyInterface(), &DestinationRect, D3DTEXF_NONE);
		}
		else if (SourceDesc.Pool == D3DPOOL_SYSTEMMEM)
		{
			const POINT pt = { DestinationRect.left, DestinationRect.top };

			hr = ProxyInterface->UpdateSurface(Source, &SourceRect, pDestinationSurfaceImpl->GetProxyInterface(), &pt);
		}

		if (FAILED(hr))
		{
#ifndef D3D8TO9NOLOG
			LOG << "Failed to translate 'IDirect3DDevice8::CopyRects' call from '[" << SourceDesc.Width << "x" << SourceDesc.Height << ", " << SourceDesc.Format << ", " << SourceDesc.MultiSampleType << ", " << SourceDesc.Usage << ", " << SourceDesc.Pool << "]' to '[" << DestinationDesc.Width << "x" << DestinationDesc.Height << ", " << DestinationDesc.Format << ", " << DestinationDesc.MultiSampleType << ", " << DestinationDesc.Usage << ", " << DestinationDesc.Pool << "]'!" << std::endl;
#endif
			break;
		}
	}
	if (Resolved)
		Resolved->Release();

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::UpdateTexture(IDirect3DBaseTexture8 *pSourceTexture, IDirect3DBaseTexture8 *pDestinationTexture)
{
	if (pSourceTexture == nullptr || pDestinationTexture == nullptr || pSourceTexture->GetType() != pDestinationTexture->GetType())
		return D3DERR_INVALIDCALL;

	IDirect3DBaseTexture9 *SourceBaseTextureInterface, *DestinationBaseTextureInterface;

	switch (pSourceTexture->GetType())
	{
	case D3DRTYPE_TEXTURE:
		SourceBaseTextureInterface = static_cast<Direct3DTexture8 *>(pSourceTexture)->GetProxyInterface();
		DestinationBaseTextureInterface = static_cast<Direct3DTexture8 *>(pDestinationTexture)->GetProxyInterface();
		break;
	case D3DRTYPE_VOLUMETEXTURE:
		SourceBaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pSourceTexture)->GetProxyInterface();
		DestinationBaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pDestinationTexture)->GetProxyInterface();
		break;
	case D3DRTYPE_CUBETEXTURE:
		SourceBaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pSourceTexture)->GetProxyInterface();
		DestinationBaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pDestinationTexture)->GetProxyInterface();
		break;
	default:
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->UpdateTexture(SourceBaseTextureInterface, DestinationBaseTextureInterface);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetFrontBuffer(IDirect3DSurface8 *pDestSurface)
{
	if (pDestSurface == nullptr)
		return D3DERR_INVALIDCALL;

	auto pDestSurfaceImpl = static_cast<Direct3DSurface8 *>(pDestSurface);
	return ProxyInterface->GetFrontBufferData(0, pDestSurfaceImpl->GetProxyInterface());
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetRenderTarget(IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pNewZStencil)
{
	HRESULT hr;

	U2.RtLeave(ProxyInterface);                     // rtdump=N (testing): a 512x512 target's picture as it is left
	if (pRenderTarget != nullptr)
	{
		auto pRenderTargetImpl = static_cast<Direct3DSurface8 *>(pRenderTarget);
		// msaa=N: a multisampled stand-in for the game's screen-sized render texture
		{
			IDirect3DSurface9 *Bind = U2.MsaaTarget(ProxyInterface, pRenderTargetImpl->GetProxyInterface());
			hr = ProxyInterface->SetRenderTarget(0, Bind);
		}
		if (FAILED(hr))
			return hr;
	}

	if (pNewZStencil != nullptr)
	{
		auto pNewZStencilImpl = static_cast<Direct3DSurface8 *>(pNewZStencil);
		// gi=1: a readable depth texture's surface in place of the game's (U2Shaders::DepthFor)
		hr = ProxyInterface->SetDepthStencilSurface(U2.DepthFor(ProxyInterface, pNewZStencilImpl->GetProxyInterface()));
		if (FAILED(hr))
			return hr;

		D3DSURFACE_DESC8 Desc = {};
		pNewZStencilImpl->GetDesc(&Desc);

		CurrentZBufferBitCount = GetDepthStencilBitCount(Desc.Format);

		ProxyInterface->SetRenderState(D3DRS_DEPTHBIAS, CalcDepthBias(CurrentZBiasRenderState, CurrentZBufferBitCount));
	}
	else
	{
		ProxyInterface->SetDepthStencilSurface(nullptr);
	}
	U2.MatchDepth(ProxyInterface);                  // msaa=N: a plain target gets a plain depth buffer

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRenderTarget(IDirect3DSurface8 **ppRenderTarget)
{
	if (ppRenderTarget == nullptr)
		return D3DERR_INVALIDCALL;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetRenderTarget(0, &SurfaceInterface);
	if (FAILED(hr))
		return hr;
	{
		IDirect3DSurface9 *Plain = U2.MsaaPlainOf(SurfaceInterface);
		if (Plain != SurfaceInterface)
		{
			Plain->AddRef();
			SurfaceInterface->Release();
			SurfaceInterface = Plain;
		}
	}

	*ppRenderTarget = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface)
{
	if (ppZStencilSurface == nullptr)
		return D3DERR_INVALIDCALL;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetDepthStencilSurface(&SurfaceInterface);
	if (FAILED(hr))
		return hr;
	// gi=1 swapped the game's depth surface for ours: the game gets its own back
	if (IDirect3DSurface9 *Game = U2.GameDepthOf(SurfaceInterface))
	{
		Game->AddRef();
		SurfaceInterface->Release();
		SurfaceInterface = Game;
	}

	*ppZStencilSurface = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::BeginScene()
{
	return ProxyInterface->BeginScene();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::EndScene()
{
	return ProxyInterface->EndScene();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
	U2.OnClear(ProxyInterface, Count, Flags);       // gi=1: a depth clear in mid-frame keeps the part drawn so far
	U2.RtClear(ProxyInterface, Count, Flags, Color);   // rtdump=N (testing)
	return ProxyInterface->Clear(Count, pRects, Flags, Color, Z, Stencil);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
	return ProxyInterface->SetTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
	return ProxyInterface->GetTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
	return ProxyInterface->MultiplyTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetViewport(const D3DVIEWPORT8 *pViewport)
{
	IDirect3DSurface9 *pCurrentRenderTarget = nullptr;
	if (SUCCEEDED(ProxyInterface->GetRenderTarget(0, &pCurrentRenderTarget)))
	{
		D3DSURFACE_DESC Desc;
		pCurrentRenderTarget->GetDesc(&Desc);

		pCurrentRenderTarget->Release();

		if (pViewport->Y + pViewport->Height > Desc.Height || pViewport->X + pViewport->Width > Desc.Width)
			return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->SetViewport(pViewport);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetViewport(D3DVIEWPORT8 *pViewport)
{
	return ProxyInterface->GetViewport(pViewport);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetMaterial(const D3DMATERIAL8 *pMaterial)
{
	return ProxyInterface->SetMaterial(pMaterial);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetMaterial(D3DMATERIAL8 *pMaterial)
{
	return ProxyInterface->GetMaterial(pMaterial);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetLight(DWORD Index, const D3DLIGHT8 *pLight)
{
	if (pLight == nullptr)
		return D3DERR_INVALIDCALL;

	D3DLIGHT8 Light = *pLight;

	// Make spot light work more like it did in Direct3D 8
	if (Light.Type == D3DLIGHTTYPE::D3DLIGHT_SPOT)
	{
		// Theta must be in the range from 0 through the value specified by Phi
		if (Light.Theta <= Light.Phi)
		{
			Light.Theta /= 1.75f;
		}
	}

	{
		D3DLIGHT9 L9;
		memcpy(&L9, &Light, sizeof(L9));
		if (U2.LightProbeFile)
			U2.LightProbeLight(ProxyInterface, Index, L9);
		U2.GiLightSet(Index, L9);                    // gi=1: the game's lights on the world cache
	}
	return ProxyInterface->SetLight(Index, &Light);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetLight(DWORD Index, D3DLIGHT8 *pLight)
{
	return ProxyInterface->GetLight(Index, pLight);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::LightEnable(DWORD Index, BOOL Enable)
{
	U2.LightProbeEnable(Index, Enable);
	U2.GiLightEnable(Index, Enable);
	return ProxyInterface->LightEnable(Index, Enable);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetLightEnable(DWORD Index, BOOL *pEnable)
{
	return ProxyInterface->GetLightEnable(Index, pEnable);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetClipPlane(DWORD Index, const float *pPlane)
{
	if (pPlane == nullptr || Index >= MAX_CLIP_PLANES)
		return D3DERR_INVALIDCALL;

	memcpy(StoredClipPlanes[Index], pPlane, sizeof(StoredClipPlanes[0]));
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetClipPlane(DWORD Index, float *pPlane)
{
	if (pPlane == nullptr || Index >= MAX_CLIP_PLANES)
		return D3DERR_INVALIDCALL;

	memcpy(pPlane, StoredClipPlanes[Index], sizeof(StoredClipPlanes[0]));
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
	HRESULT hr;

	// msaa=N: the game's own "no multisampling" doesn't apply
	if ((State == D3DRS_MULTISAMPLEANTIALIAS || State == D3DRS_MULTISAMPLEMASK) && U2Msaa::Wanted() != 0)
	{
		static int Told = 0;
		if (Told++ < 8)
			U2FakeFull::Note(State == D3DRS_MULTISAMPLEANTIALIAS ? "msaa: the game sets MULTISAMPLEANTIALIAS (kept on)" : "msaa: the game sets MULTISAMPLEMASK (kept all samples)");
		Value = State == D3DRS_MULTISAMPLEANTIALIAS ? TRUE : 0xFFFFFFFF;
	}

	switch (static_cast<DWORD>(State))
	{
	case D3DRS_ZVISIBLE:
	case D3DRS_PATCHSEGMENTS:
	case D3DRS_LINEPATTERN:
		return D3D_OK;
	case D3DRS_SOFTWAREVERTEXPROCESSING:
		// SWVP can be modified by this render state only on devices
		// created with the D3DCREATE_MIXED_VERTEXPROCESSING flag
		if (IsMixedVertexProcessingDevice)
			return ProxyInterface->SetSoftwareVertexProcessing(static_cast<BOOL>(Value));
		return D3D_OK;
	case D3DRS_EDGEANTIALIAS:
		return ProxyInterface->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, Value);
	case D3DRS_CLIPPLANEENABLE:
		hr = ProxyInterface->SetRenderState(State, Value);
		if (SUCCEEDED(hr))
			ClipPlaneRenderState = Value;
		return hr;
	case D3DRS_ZBIAS:
		CurrentZBiasRenderState = Value;
		Value = CalcDepthBias(Value, CurrentZBufferBitCount);
		State = D3DRS_DEPTHBIAS;
	default:
		return ProxyInterface->SetRenderState(State, Value);
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue)
{
	if (pValue == nullptr)
		return D3DERR_INVALIDCALL;

	*pValue = 0;

	switch (static_cast<DWORD>(State))
	{
	case D3DRS_ZVISIBLE:
	case D3DRS_LINEPATTERN:
		*pValue = 0;
		return D3D_OK;
	case D3DRS_EDGEANTIALIAS:
		return ProxyInterface->GetRenderState(D3DRS_ANTIALIASEDLINEENABLE, pValue);
	case D3DRS_ZBIAS:
		*pValue = CurrentZBiasRenderState;
		return D3D_OK;
	case D3DRS_SOFTWAREVERTEXPROCESSING:
		*pValue = static_cast<DWORD>(ProxyInterface->GetSoftwareVertexProcessing());
		return D3D_OK;
	case D3DRS_PATCHSEGMENTS:
		*pValue = 1;
		return D3D_OK;
	default:
		return ProxyInterface->GetRenderState(State, pValue);
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::BeginStateBlock()
{
	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	HRESULT hr = ProxyInterface->BeginStateBlock();

	if (SUCCEEDED(hr))
		IsRecordingState = true;

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::EndStateBlock(DWORD *pToken)
{
	if (pToken == nullptr)
		return D3DERR_INVALIDCALL;

	if (!IsRecordingState)
		return D3DERR_INVALIDCALL;

	HRESULT hr = ProxyInterface->EndStateBlock(reinterpret_cast<IDirect3DStateBlock9**>(pToken));

	if (SUCCEEDED(hr))
	{
		StateBlockTokens.insert(*pToken);
		IsRecordingState = false;
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::ApplyStateBlock(DWORD Token)
{
	if (Token == 0)
		return D3DERR_INVALIDCALL;

	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	if (StateBlockTokens.find(Token) == StateBlockTokens.end())
		return D3D_OK;

	return reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Apply();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CaptureStateBlock(DWORD Token)
{
	if (Token == 0)
		return D3DERR_INVALIDCALL;

	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	if (StateBlockTokens.find(Token) == StateBlockTokens.end())
		return D3D_OK;

	return reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Capture();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeleteStateBlock(DWORD Token)
{
	if (Token == 0)
		return D3DERR_INVALIDCALL;

	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	if (StateBlockTokens.find(Token) == StateBlockTokens.end())
		return D3D_OK;

	reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Release();

	StateBlockTokens.erase(Token);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD *pToken)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateStateBlock" << "(" << Type << ", " << pToken << ")' ..." << std::endl;
#endif

	if (pToken == nullptr)
		return D3DERR_INVALIDCALL;

	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	HRESULT hr = ProxyInterface->CreateStateBlock(Type, reinterpret_cast<IDirect3DStateBlock9 **>(pToken));

	if (SUCCEEDED(hr))
		StateBlockTokens.insert(*pToken);

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetClipStatus(const D3DCLIPSTATUS8 *pClipStatus)
{
	return ProxyInterface->SetClipStatus(pClipStatus);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetClipStatus(D3DCLIPSTATUS8 *pClipStatus)
{
	return ProxyInterface->GetClipStatus(pClipStatus);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTexture(DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
	if (ppTexture == nullptr)
		return D3DERR_INVALIDCALL;

	*ppTexture = nullptr;

	IDirect3DBaseTexture9 *BaseTextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetTexture(Stage, &BaseTextureInterface);
	if (FAILED(hr))
		return hr;

	if (BaseTextureInterface != nullptr)
	{
		IDirect3DTexture9 *TextureInterface = nullptr;
		IDirect3DCubeTexture9 *CubeTextureInterface = nullptr;
		IDirect3DVolumeTexture9 *VolumeTextureInterface = nullptr;

		switch (BaseTextureInterface->GetType())
		{
		case D3DRTYPE_TEXTURE:
			BaseTextureInterface->QueryInterface(IID_PPV_ARGS(&TextureInterface));
			*ppTexture = ProxyAddressLookupTable->FindAddress<Direct3DTexture8>(TextureInterface);
			BaseTextureInterface->Release();
			break;
		case D3DRTYPE_VOLUMETEXTURE:
			BaseTextureInterface->QueryInterface(IID_PPV_ARGS(&VolumeTextureInterface));
			*ppTexture = ProxyAddressLookupTable->FindAddress<Direct3DVolumeTexture8>(VolumeTextureInterface);
			BaseTextureInterface->Release();
			break;
		case D3DRTYPE_CUBETEXTURE:
			BaseTextureInterface->QueryInterface(IID_PPV_ARGS(&CubeTextureInterface));
			*ppTexture = ProxyAddressLookupTable->FindAddress<Direct3DCubeTexture8>(CubeTextureInterface);
			BaseTextureInterface->Release();
			break;
		default:
			BaseTextureInterface->Release();
			return D3DERR_INVALIDCALL;
		}
	}

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTexture(DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
	if (Stage == 0)
		U2Stage0 = (pTexture != nullptr && pTexture->GetType() == D3DRTYPE_TEXTURE) ? static_cast<Direct3DTexture8 *>(pTexture) : nullptr;
	if (Stage < 4)
		U2Stages[Stage] = (pTexture != nullptr && pTexture->GetType() == D3DRTYPE_TEXTURE) ? static_cast<Direct3DTexture8 *>(pTexture) : nullptr;

	if (pTexture == nullptr)
		return ProxyInterface->SetTexture(Stage, nullptr);

	IDirect3DBaseTexture9 *BaseTextureInterface;

	switch (pTexture->GetType())
	{
	case D3DRTYPE_TEXTURE:
		BaseTextureInterface = static_cast<Direct3DTexture8 *>(pTexture)->GetProxyInterface();
		break;
	case D3DRTYPE_VOLUMETEXTURE:
		BaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pTexture)->GetProxyInterface();
		break;
	case D3DRTYPE_CUBETEXTURE:
		BaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pTexture)->GetProxyInterface();
		break;
	default:
		return D3DERR_INVALIDCALL;
	}
	U2.MsaaBeforeRead(ProxyInterface, BaseTextureInterface, nullptr);

	return ProxyInterface->SetTexture(Stage, BaseTextureInterface);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
	switch (static_cast<DWORD>(Type))
	{
	case D3DTSS_ADDRESSU:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_ADDRESSU, pValue);
	case D3DTSS_ADDRESSV:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_ADDRESSV, pValue);
	case D3DTSS_ADDRESSW:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_ADDRESSW, pValue);
	case D3DTSS_BORDERCOLOR:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_BORDERCOLOR, pValue);
	case D3DTSS_MAGFILTER:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MAGFILTER, pValue);
	case D3DTSS_MINFILTER:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MINFILTER, pValue);
	case D3DTSS_MIPFILTER:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MIPFILTER, pValue);
	case D3DTSS_MIPMAPLODBIAS:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MIPMAPLODBIAS, pValue);
	case D3DTSS_MAXMIPLEVEL:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MAXMIPLEVEL, pValue);
	case D3DTSS_MAXANISOTROPY:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MAXANISOTROPY, pValue);
	default:
		return ProxyInterface->GetTextureStageState(Stage, Type, pValue);
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
	switch (static_cast<DWORD>(Type))
	{
	case D3DTSS_ADDRESSU:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_ADDRESSU, Value);
	case D3DTSS_ADDRESSV:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_ADDRESSV, Value);
	case D3DTSS_ADDRESSW:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_ADDRESSW, Value);
	case D3DTSS_BORDERCOLOR:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_BORDERCOLOR, Value);
	case D3DTSS_MAGFILTER:
		if (Value == D3DTEXF_FLATCUBIC || Value == D3DTEXF_GAUSSIANCUBIC)
			Value = D3DTEXF_LINEAR;
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MAGFILTER, Value);
	case D3DTSS_MINFILTER:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MINFILTER, Value);
	case D3DTSS_MIPFILTER:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MIPFILTER, Value);
	case D3DTSS_MIPMAPLODBIAS:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MIPMAPLODBIAS, Value);
	case D3DTSS_MAXMIPLEVEL:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MAXMIPLEVEL, Value);
	case D3DTSS_MAXANISOTROPY:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MAXANISOTROPY, Value);
	default:
		return ProxyInterface->SetTextureStageState(Stage, Type, Value);
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::ValidateDevice(DWORD *pNumPasses)
{
	return ProxyInterface->ValidateDevice(pNumPasses);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetInfo(DWORD DevInfoID, void *pDevInfoStruct, DWORD DevInfoStructSize)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetInfo" << "(" << this << ", " << DevInfoID << ", " << pDevInfoStruct << ", " << DevInfoStructSize << ")' ..." << std::endl;
#endif

	if (pDevInfoStruct == nullptr || DevInfoStructSize == 0)
		return D3DERR_INVALIDCALL;

	HRESULT hr;
	IDirect3DQuery9 *pQuery = nullptr;

	switch (DevInfoID)
	{
		case 0:
		case D3DDEVINFOID_TEXTUREMANAGER:
		case D3DDEVINFOID_D3DTEXTUREMANAGER:
		case D3DDEVINFOID_TEXTURING:
			return E_FAIL; // Unsupported query IDs

		case D3DDEVINFOID_VCACHE:
			hr = ProxyInterface->CreateQuery(D3DQUERYTYPE_VCACHE, &pQuery);

			if (FAILED(hr))
			{
				if (DevInfoStructSize != sizeof(D3DDEVINFO_VCACHE))
					return D3DERR_INVALIDCALL;

				// The contents of pDevInfoStruct are zeroed before return
				memset(pDevInfoStruct, 0, sizeof(D3DDEVINFO_VCACHE));
				return S_FALSE;
			}

			break;

		case D3DDEVINFOID_RESOURCEMANAGER:
			hr = ProxyInterface->CreateQuery(D3DQUERYTYPE_RESOURCEMANAGER, &pQuery);
			break;

		case D3DDEVINFOID_VERTEXSTATS:
			hr = ProxyInterface->CreateQuery(D3DQUERYTYPE_VERTEXSTATS, &pQuery);
			break;

		default: // D3DDEVINFOID_UNKNOWN
			return E_FAIL;
	}

	if ((FAILED(hr)))
	{
		if (hr == D3DERR_NOTAVAILABLE)
		{
			return E_FAIL;
		}
		else
		{
			return S_FALSE;
		}
	}

	if (pQuery != nullptr)
	{
		pQuery->Issue(D3DISSUE_END);
		hr = pQuery->GetData(pDevInfoStruct, DevInfoStructSize, D3DGETDATA_FLUSH);

		pQuery->Release();
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries)
{
	if (pEntries == nullptr)
		return D3DERR_INVALIDCALL;

	return ProxyInterface->SetPaletteEntries(PaletteNumber, pEntries);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries)
{
	if (pEntries == nullptr)
		return D3DERR_INVALIDCALL;

	return ProxyInterface->GetPaletteEntries(PaletteNumber, pEntries);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetCurrentTexturePalette(UINT PaletteNumber)
{
	if (!IsPaletteSupported)
		return D3DERR_INVALIDCALL;

	return ProxyInterface->SetCurrentTexturePalette(PaletteNumber);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetCurrentTexturePalette(UINT *pPaletteNumber)
{
	if (!IsPaletteSupported)
		return D3DERR_INVALIDCALL;

	return ProxyInterface->GetCurrentTexturePalette(pPaletteNumber);
}
bool Direct3DDevice8::U2Begin()
{
	// a D3D8 "vertex shader" without code is just a vertex layout: still fixed function
	bool FixedFunction = CurrentVertexShaderHandle == 0;
	if (!FixedFunction)
		FixedFunction = reinterpret_cast<VertexShaderInfo *>(CurrentVertexShaderHandle << 1)->Shader == nullptr;
	// replace=: our textures in place of the game's for this draw (U2After puts them back)
	U2Swapped = 0;
	for (DWORD s = 0; s < 4; s++)
		if (U2Stages[s] != nullptr)
			if (IDirect3DTexture9 *R = U2.Replacement(ProxyInterface, U2Stages[s]->GetProxyInterface(), U2Stages[s]->U2Hash))
			{
				ProxyInterface->SetTexture(s, R);
				U2Swapped |= 1u << s;
			}
	U2.LogTargetDraw(ProxyInterface, FixedFunction, U2Stage0 != nullptr);
	if (U2.StageLog)
	{
		// stagelog=1: each distinct on-screen draw setup once: what sits on stages 0-3 (2D texture
		// hash, "cube", "none") with the colour ops, to find draws the texture rules can't see
		char Key[400];
		int n = 0;
		for (DWORD s = 0; s < 4; s++)
		{
			IDirect3DBaseTexture9 *T = nullptr;
			ProxyInterface->GetTexture(s, &T);
			DWORD Op = 0;
			ProxyInterface->GetTextureStageState(s, D3DTSS_COLOROP, &Op);
			if (T == nullptr)
				n += sprintf_s(Key + n, sizeof(Key) - n, "st%u none op%u | ", s, Op);
			else
			{
				if (T->GetType() == D3DRTYPE_CUBETEXTURE)
					n += sprintf_s(Key + n, sizeof(Key) - n, "st%u cube op%u | ", s, Op);
				else
				{
					if (U2Stages[s] != nullptr)
						U2.Known(U2Stages[s]->GetProxyInterface(), U2Stages[s]->U2Hash);
					n += sprintf_s(Key + n, sizeof(Key) - n, "st%u %08x op%u | ", s, U2Stages[s] ? U2Stages[s]->U2Hash : 0, Op);
				}
				T->Release();
			}
			if (Op == D3DTOP_DISABLE)
				break;
		}
		DWORD Blend = 0, Src = 0, Dst = 0;
		ProxyInterface->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
		ProxyInterface->GetRenderState(D3DRS_SRCBLEND, &Src);
		ProxyInterface->GetRenderState(D3DRS_DESTBLEND, &Dst);
		D3DVIEWPORT9 VP = {};
		ProxyInterface->GetViewport(&VP);
		sprintf_s(Key + n, sizeof(Key) - n, "blend %u %u/%u ff %d z %.2f-%.2f", Blend, Src, Dst, FixedFunction, VP.MinZ, VP.MaxZ);
		U2.StageLogDraw(ProxyInterface, Key);
	}
	// pbr=: the rule's log line names the game's pixel shader for the draw
	U2.CurPsHash = 0;
	if (CurrentPixelShaderHandle != 0)
	{
		auto PsIt = U2PsHash.find(CurrentPixelShaderHandle);
		if (PsIt != U2PsHash.end())
			U2.CurPsHash = PsIt->second;
	}
	U2.RelightBegin(ProxyInterface, U2Stage0 != nullptr, FixedFunction);
	U2.LightProbeDraw(ProxyInterface, U2Stage0 ? U2Stage0->U2Hash : 0, U2Stage0 != nullptr, FixedFunction);
	if (CurrentPixelShaderHandle != 0 && !U2.PsReplace.empty())
	{
		auto It = U2PsHash.find(CurrentPixelShaderHandle);
		if (It != U2PsHash.end() && U2.PsReplaceBegin(ProxyInterface, It->second))
			return true;
	}
	if (U2.PcssBegin(ProxyInterface, U2Stage0 ? U2Stage0->GetProxyInterface() : nullptr, FixedFunction))
		return true;
	if (U2Stage0 == nullptr)
	{
		// an environment-mapped surface (a cube map on stage 0, its texture on stage 1, e.g. the
		// Bio Rifle's glass canister): a rule on the stage-1 texture takes it. The shader reads
		// its texture from s0, so that texture is put there for the draw (U2After restores the cube)
		IDirect3DBaseTexture9 *T0 = nullptr;
		ProxyInterface->GetTexture(0, &T0);
		const bool Cube = T0 && T0->GetType() == D3DRTYPE_CUBETEXTURE;
		if (Cube && U2Stages[1] == nullptr)
		{
			// a cube map alone (an environment-mapped surface with no texture of its own)
			DWORD H = CubeOnlyHash;
			const bool Taken = U2.Begin(ProxyInterface, nullptr, H, FixedFunction);
			T0->Release();
			return Taken;
		}
		if (!Cube || U2Stages[1] == nullptr)
		{
			if (T0) T0->Release();
			return false;
		}
		ProxyInterface->SetTexture(0, U2Stages[1]->GetProxyInterface());
		if (U2.Begin(ProxyInterface, U2Stages[1]->GetProxyInterface(), U2Stages[1]->U2Hash, FixedFunction))
		{
			U2Cube0 = T0;                    // keeps the reference until U2After
			return true;
		}
		ProxyInterface->SetTexture(0, T0);
		T0->Release();
		return false;
	}
	return U2.Begin(ProxyInterface, U2Stage0->GetProxyInterface(), U2Stage0->U2Hash, FixedFunction);
}
// lmcapture=: hands a draw's vertices (from the buffers' copies) to U2Shaders::CaptureDraw
void Direct3DDevice8::U2CaptureDraw(D3DPRIMITIVETYPE Type, UINT PrimCount, const BYTE *Verts, size_t VertBytes, UINT Stride, INT BaseVertex,
	const BYTE *Indices, size_t IndexBytes, bool Index32, UINT StartIndex)
{
	if (U2Stages[1] == nullptr || Verts == nullptr)
		return;
	U2.CaptureDraw(ProxyInterface, Type, PrimCount, Verts, VertBytes, Stride, BaseVertex, Indices, IndexBytes, Index32, StartIndex,
		U2Stages[0] ? U2Stages[0]->GetProxyInterface() : nullptr, U2Stages[0] ? &U2Stages[0]->U2Hash : nullptr,
		U2Stages[1]->GetProxyInterface(), U2Stages[1]->U2Hash);
}
bool Direct3DDevice8::U2MaskBegin(UINT Prims)
{
	if (U2.MaskState != 1)
		return false;
	bool FixedFunction = CurrentVertexShaderHandle == 0;
	if (!FixedFunction)
		FixedFunction = reinterpret_cast<VertexShaderInfo *>(CurrentVertexShaderHandle << 1)->Shader == nullptr;
	return U2.MaskBegin(ProxyInterface, FixedFunction, U2Stage0 != nullptr, U2Stage0 ? U2Stage0->U2Hash : 0, Prims);
}
bool Direct3DDevice8::U2StreakBegin(int Pass)
{
	if (!U2Streaks::Any())
		return false;
	bool FixedFunction = CurrentVertexShaderHandle == 0;
	if (!FixedFunction)
		FixedFunction = reinterpret_cast<VertexShaderInfo *>(CurrentVertexShaderHandle << 1)->Shader == nullptr;
	return U2.StreakBegin(ProxyInterface, Pass, FixedFunction, U2Stage0 != nullptr);
}
bool Direct3DDevice8::U2GlossBegin()
{
	if (U2.GlossRules.empty() || U2Stage0 == nullptr)
		return false;
	bool FixedFunction = CurrentVertexShaderHandle == 0;
	if (!FixedFunction)
		FixedFunction = reinterpret_cast<VertexShaderInfo *>(CurrentVertexShaderHandle << 1)->Shader == nullptr;
	return U2.GlossBegin(ProxyInterface, U2Stage0->GetProxyInterface(), U2Stage0->U2Hash, FixedFunction);
}
void Direct3DDevice8::U2After(bool Shaded)
{
	U2.RelightEnd(ProxyInterface);
	if (Shaded)
		U2.End(ProxyInterface);
	for (DWORD s = 0; s < 4; s++)
		if ((U2Swapped & (1u << s)) && U2Stages[s] != nullptr)
			ProxyInterface->SetTexture(s, U2Stages[s]->GetProxyInterface());
	U2Swapped = 0;
	if (U2Cube0 != nullptr)
	{
		ProxyInterface->SetTexture(0, U2Cube0);
		U2Cube0->Release();
		U2Cube0 = nullptr;
	}
}
// lightprobe: a draw's vertex layout and the average length of its normals (0 = no lighting possible)
static void U2ProbeVB(IDirect3DDevice9 *Dev, const BYTE *Data, size_t Size, UINT Stride, size_t First, UINT Count)
{
	if (!U2.LightProbeFile || Data == nullptr || Stride == 0)
		return;
	int NormOff = -1;
	char Decl[200] = "";
	IDirect3DVertexDeclaration9 *VD = nullptr;
	if (SUCCEEDED(Dev->GetVertexDeclaration(&VD)) && VD)
	{
		D3DVERTEXELEMENT9 E[MAXD3DDECLLENGTH]; UINT N = MAXD3DDECLLENGTH;
		if (SUCCEEDED(VD->GetDeclaration(E, &N)))
			for (UINT e = 0; e + 1 < N && strlen(Decl) < 170; e++)
			{
				char b[24]; sprintf_s(b, " s%u@%u:t%u/u%u", E[e].Stream, E[e].Offset, E[e].Type, E[e].Usage); strcat_s(Decl, b);
				if (E[e].Stream == 0 && E[e].Usage == D3DDECLUSAGE_NORMAL && E[e].Type == D3DDECLTYPE_FLOAT3) NormOff = E[e].Offset;
			}
		VD->Release();
	}
	double Sum = 0; UINT Cnt = 0;
	if (NormOff >= 0)
		for (UINT i = 0; i < Count; i++)
		{
			const size_t At = (First + i) * Stride + NormOff;
			if (At + 12 > Size) break;
			const float *Nf = (const float *)(Data + At);
			Sum += sqrt((double)Nf[0] * Nf[0] + (double)Nf[1] * Nf[1] + (double)Nf[2] * Nf[2]); Cnt++;
		}
	fprintf(U2.LightProbeFile, "  (prev) decl%s | normal at %d, avg |n| %.3f over %u verts, stride %u\n", Decl, NormOff, Cnt ? Sum / Cnt : -1.0, Cnt, Stride);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
	ApplyClipPlanes();
	if (U2.Capture && U2Stream0 != nullptr && !U2Stream0->U2Shadow.empty() && (size_t)StartVertex * U2Stride0 < U2Stream0->U2Shadow.size())
		U2CaptureDraw(PrimitiveType, PrimitiveCount, U2Stream0->U2Shadow.data() + (size_t)StartVertex * U2Stride0,
			U2Stream0->U2Shadow.size() - (size_t)StartVertex * U2Stride0, U2Stride0, 0, nullptr, 0, false, 0);
	const bool Shaded = U2Begin();
	U2.LastDrawHR = ProxyInterface->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
	if (U2.LightProbeFile && U2Stream0 != nullptr && !U2Stream0->U2Shadow.empty())
		U2ProbeVB(ProxyInterface, U2Stream0->U2Shadow.data(), U2Stream0->U2Shadow.size(), U2Stride0, StartVertex,
			PrimitiveType == D3DPT_TRIANGLELIST ? PrimitiveCount * 3 : PrimitiveCount + 2);
	U2After(Shaded);
	if (U2GlossBegin())                             // gloss=: the same geometry again, added on top
	{
		ProxyInterface->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		U2.GlossEnd(ProxyInterface);
	}
	for (int Pass = 0; U2StreakBegin(Pass); Pass++)   // streaks=1: blood running down characters (a pass per two sources)
	{
		ProxyInterface->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		U2Streaks::End(ProxyInterface);
	}
	if (U2MaskBegin(PrimitiveCount))                // shotmask=1: the same geometry into the character mask
	{
		ProxyInterface->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		U2.MaskEnd(ProxyInterface);
	}
	if (TexEd().PickBegin(U2, ProxyInterface, U2Stages))   // texedit: the click frame's pick pass
	{
		ProxyInterface->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		TexEd().PickEnd(ProxyInterface);
	}
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
	ApplyClipPlanes();
	if (U2.Capture && U2Stream0 != nullptr && U2Indices != nullptr && !U2Stream0->U2Shadow.empty() && !U2Indices->U2Shadow.empty())
	{
		D3DINDEXBUFFER_DESC IbDesc;
		U2Indices->GetProxyInterface()->GetDesc(&IbDesc);
		U2CaptureDraw(PrimitiveType, PrimitiveCount, U2Stream0->U2Shadow.data(), U2Stream0->U2Shadow.size(), U2Stride0, CurrentBaseVertexIndex,
			U2Indices->U2Shadow.data(), U2Indices->U2Shadow.size(), IbDesc.Format == D3DFMT_INDEX32, StartIndex);
	}
	if (U2.LightProbeFile && U2Stream0 != nullptr && U2Indices != nullptr && !U2Stream0->U2Shadow.empty() && !U2Indices->U2Shadow.empty())
	{
		// lightprobe: the first vertex this draw uses, raw (to compare normals between the shadow pass and the view)
		D3DINDEXBUFFER_DESC IbDesc;
		U2Indices->GetProxyInterface()->GetDesc(&IbDesc);
		const bool I32 = IbDesc.Format == D3DFMT_INDEX32;
		const size_t Off = (size_t)StartIndex * (I32 ? 4 : 2);
		if (Off + 4 <= U2Indices->U2Shadow.size())
		{
			const UINT Idx = I32 ? *(const UINT *)(U2Indices->U2Shadow.data() + Off) : *(const WORD *)(U2Indices->U2Shadow.data() + Off);
			const size_t V = ((size_t)CurrentBaseVertexIndex + Idx) * U2Stride0;
			// the vertex layout (declaration): where the normal is, then its average length over the draw
			int NormOff = -1;
			char Decl[200] = "";
			IDirect3DVertexDeclaration9 *VD = nullptr;
			if (SUCCEEDED(ProxyInterface->GetVertexDeclaration(&VD)) && VD)
			{
				D3DVERTEXELEMENT9 E[MAXD3DDECLLENGTH]; UINT N = MAXD3DDECLLENGTH;
				if (SUCCEEDED(VD->GetDeclaration(E, &N)))
					for (UINT e = 0; e + 1 < N && strlen(Decl) < 170; e++)
					{
						char b[24]; sprintf_s(b, " s%u@%u:t%u/u%u", E[e].Stream, E[e].Offset, E[e].Type, E[e].Usage); strcat_s(Decl, b);
						if (E[e].Stream == 0 && E[e].Usage == D3DDECLUSAGE_NORMAL && E[e].Type == D3DDECLTYPE_FLOAT3) NormOff = E[e].Offset;
					}
				VD->Release();
			}
			double Sum = 0; UINT Cnt = 0;
			if (NormOff >= 0)
				for (UINT i = 0; i < NumVertices; i++)
				{
					const size_t At = ((size_t)CurrentBaseVertexIndex + MinIndex + i) * U2Stride0 + NormOff;
					if (At + 12 > U2Stream0->U2Shadow.size()) break;
					const float *Nf = (const float *)(U2Stream0->U2Shadow.data() + At);
					Sum += sqrt((double)Nf[0] * Nf[0] + (double)Nf[1] * Nf[1] + (double)Nf[2] * Nf[2]); Cnt++;
				}
			fprintf(U2.LightProbeFile, "  decl%s | normal at %d, avg |n| %.3f over %u verts, stride %u\n", Decl, NormOff, Cnt ? Sum / Cnt : -1.0, Cnt, U2Stride0);
		}
	}
	const bool Shaded = U2Begin();
	U2.LastDrawHR = ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount);
	U2After(Shaded);
	if (U2GlossBegin())                             // gloss=: the same geometry again, added on top
	{
		ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount);
		U2.GlossEnd(ProxyInterface);
	}
	for (int Pass = 0; U2StreakBegin(Pass); Pass++)   // streaks=1: blood running down characters (a pass per two sources)
	{
		ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount);
		U2Streaks::End(ProxyInterface);
	}
	if (U2MaskBegin(PrimitiveCount))                // shotmask=1: the same geometry into the character mask
	{
		ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount);
		U2.MaskEnd(ProxyInterface);
	}
	if (TexEd().PickBegin(U2, ProxyInterface, U2Stages))   // texedit: the click frame's pick pass
	{
		ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount);
		TexEd().PickEnd(ProxyInterface);
	}
	return D3D_OK;
}
// lightprobe: a user-pointer draw's vertices: the first one raw, and the average length of floats 3-5
// (the normal, in the usual position + normal + uv layout) over the vertices it uses
static void U2ProbeVerts(const void *Data, UINT Stride, UINT First, UINT Count)
{
	if (!U2.LightProbeFile || Data == nullptr || Stride < 32 || Count == 0)
		return;
	double Sum = 0;
	for (UINT i = 0; i < Count; i++)
	{
		const float *F = (const float *)((const BYTE *)Data + (size_t)(First + i) * Stride);
		Sum += sqrt((double)F[3] * F[3] + (double)F[4] * F[4] + (double)F[5] * F[5]);
	}
	const float *F = (const float *)((const BYTE *)Data + (size_t)First * Stride);
	fprintf(U2.LightProbeFile, "  UP stride %u verts %u v0 %.1f %.1f %.1f | n %.3f %.3f %.3f | %.3f %.3f | avg |n| %.3f\n", Stride, Count,
		F[0], F[1], F[2], F[3], F[4], F[5], F[6], F[7], Sum / Count);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	ApplyClipPlanes();
	if (U2.Capture)
		U2CaptureDraw(PrimitiveType, PrimitiveCount, static_cast<const BYTE *>(pVertexStreamZeroData), (size_t)(PrimitiveType == D3DPT_TRIANGLELIST ? PrimitiveCount * 3 : PrimitiveCount + 2) * VertexStreamZeroStride,
			VertexStreamZeroStride, 0, nullptr, 0, false, 0);
	const bool Shaded = U2Begin();
	U2.LastDrawHR = ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
	U2ProbeVerts(pVertexStreamZeroData, VertexStreamZeroStride, 0, PrimitiveType == D3DPT_TRIANGLELIST ? PrimitiveCount * 3 : PrimitiveCount + 2);
	U2After(Shaded);
	if (U2GlossBegin())                             // gloss=: the same geometry again, added on top
	{
		ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		U2.GlossEnd(ProxyInterface);
	}
	for (int Pass = 0; U2StreakBegin(Pass); Pass++)   // streaks=1: blood running down characters (a pass per two sources)
	{
		ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		U2Streaks::End(ProxyInterface);
	}
	if (U2MaskBegin(PrimitiveCount))                // shotmask=1: the same geometry into the character mask
	{
		ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		U2.MaskEnd(ProxyInterface);
	}
	if (TexEd().PickBegin(U2, ProxyInterface, U2Stages))   // texedit: the click frame's pick pass
	{
		ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		TexEd().PickEnd(ProxyInterface);
	}
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	ApplyClipPlanes();
	if (U2.Capture)
		U2CaptureDraw(PrimitiveType, PrimitiveCount, static_cast<const BYTE *>(pVertexStreamZeroData), (size_t)(MinVertexIndex + NumVertexIndices) * VertexStreamZeroStride,
			VertexStreamZeroStride, 0, static_cast<const BYTE *>(pIndexData), (size_t)(PrimitiveType == D3DPT_TRIANGLELIST ? PrimitiveCount * 3 : PrimitiveCount + 2) * (IndexDataFormat == D3DFMT_INDEX32 ? 4 : 2),
			IndexDataFormat == D3DFMT_INDEX32, 0);
	const bool Shaded = U2Begin();
	U2.LastDrawHR = ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
	U2ProbeVerts(pVertexStreamZeroData, VertexStreamZeroStride, MinVertexIndex, NumVertexIndices);
	U2After(Shaded);
	if (U2GlossBegin())                             // gloss=: the same geometry again, added on top
	{
		ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		U2.GlossEnd(ProxyInterface);
	}
	for (int Pass = 0; U2StreakBegin(Pass); Pass++)   // streaks=1: blood running down characters (a pass per two sources)
	{
		ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		U2Streaks::End(ProxyInterface);
	}
	if (U2MaskBegin(PrimitiveCount))                // shotmask=1: the same geometry into the character mask
	{
		ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		U2.MaskEnd(ProxyInterface);
	}
	if (TexEd().PickBegin(U2, ProxyInterface, U2Stages))   // texedit: the click frame's pick pass
	{
		ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		TexEd().PickEnd(ProxyInterface);
	}
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags)
{
	if (pDestBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	Direct3DVertexBuffer8 *pDestBufferImpl = static_cast<Direct3DVertexBuffer8 *>(pDestBuffer);
	return ProxyInterface->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBufferImpl->GetProxyInterface(), nullptr, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVertexShader(const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
{
	UNREFERENCED_PARAMETER(Usage);

#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateVertexShader" << "(" << this << ", " << pDeclaration << ", " << pFunction << ", " << pHandle << ", " << Usage << ")' ..." << std::endl;
#endif

	if (pDeclaration == nullptr || pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	*pHandle = 0;

	UINT ElementIndex = 0;
	const UINT ElementLimit = 32;
	std::string ConstantsCode;
	WORD Stream = 0, Offset = 0;
	DWORD VertexShaderInputs[ElementLimit];
	D3DVERTEXELEMENT9 VertexElements[ElementLimit];

#ifndef D3D8TO9NOLOG
	LOG << "> Translating vertex declaration ..." << std::endl;
#endif

	static const BYTE DeclTypes[][2] =
	{
		{ D3DDECLTYPE_FLOAT1, 4 },
		{ D3DDECLTYPE_FLOAT2, 8 },
		{ D3DDECLTYPE_FLOAT3, 12 },
		{ D3DDECLTYPE_FLOAT4, 16 },
		{ D3DDECLTYPE_D3DCOLOR, 4 },
		{ D3DDECLTYPE_UBYTE4, 4 },
		{ D3DDECLTYPE_SHORT2, 4 },
		{ D3DDECLTYPE_SHORT4, 8 },
		{ D3DDECLTYPE_UBYTE4N, 4 },
		{ D3DDECLTYPE_SHORT2N, 4 },
		{ D3DDECLTYPE_SHORT4N, 8 },
		{ D3DDECLTYPE_USHORT2N, 4 },
		{ D3DDECLTYPE_USHORT4N, 8 },
		{ D3DDECLTYPE_UDEC3, 6 },
		{ D3DDECLTYPE_DEC3N, 6 },
		{ D3DDECLTYPE_FLOAT16_2, 8 },
		{ D3DDECLTYPE_FLOAT16_4, 16 }
	};
	static const BYTE DeclAddressUsages[][2] =
	{
		{ D3DDECLUSAGE_POSITION, 0 },
		{ D3DDECLUSAGE_BLENDWEIGHT, 0 },
		{ D3DDECLUSAGE_BLENDINDICES, 0 },
		{ D3DDECLUSAGE_NORMAL, 0 },
		{ D3DDECLUSAGE_PSIZE, 0 },
		{ D3DDECLUSAGE_COLOR, 0 },
		{ D3DDECLUSAGE_COLOR, 1 },
		{ D3DDECLUSAGE_TEXCOORD, 0 },
		{ D3DDECLUSAGE_TEXCOORD, 1 },
		{ D3DDECLUSAGE_TEXCOORD, 2 },
		{ D3DDECLUSAGE_TEXCOORD, 3 },
		{ D3DDECLUSAGE_TEXCOORD, 4 },
		{ D3DDECLUSAGE_TEXCOORD, 5 },
		{ D3DDECLUSAGE_TEXCOORD, 6 },
		{ D3DDECLUSAGE_TEXCOORD, 7 },
		{ D3DDECLUSAGE_POSITION, 1 },
		{ D3DDECLUSAGE_NORMAL, 1 }
	};

	while (ElementIndex < ElementLimit)
	{
		const DWORD Token = *pDeclaration;
		const DWORD TokenType = (Token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		if (Token == D3DVSD_END())
		{
			break;
		}
		else if (TokenType == D3DVSD_TOKEN_STREAM)
		{
			Stream = static_cast<WORD>((Token & D3DVSD_STREAMNUMBERMASK) >> D3DVSD_STREAMNUMBERSHIFT);
			Offset = 0;
		}
		else if (TokenType == D3DVSD_TOKEN_STREAMDATA && !(Token & 0x10000000))
		{
			VertexElements[ElementIndex].Stream = Stream;
			VertexElements[ElementIndex].Offset = Offset;
			const DWORD type = (Token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT;
			VertexElements[ElementIndex].Type = DeclTypes[type][0];
			Offset += DeclTypes[type][1];
			VertexElements[ElementIndex].Method = D3DDECLMETHOD_DEFAULT;
			const DWORD Address = (Token & D3DVSD_VERTEXREGMASK) >> D3DVSD_VERTEXREGSHIFT;
			VertexElements[ElementIndex].Usage = DeclAddressUsages[Address][0];
			VertexElements[ElementIndex].UsageIndex = DeclAddressUsages[Address][1];

			VertexShaderInputs[ElementIndex++] = Address;
		}
		else if (TokenType == D3DVSD_TOKEN_STREAMDATA && (Token & 0x10000000))
		{
			Offset += ((Token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT) * sizeof(DWORD);
		}
		else if (TokenType == D3DVSD_TOKEN_TESSELLATOR && !(Token & 0x10000000))
		{
			VertexElements[ElementIndex].Stream = Stream;
			VertexElements[ElementIndex].Offset = Offset;

			const DWORD UsageType = (Token & D3DVSD_VERTEXREGINMASK) >> D3DVSD_VERTEXREGINSHIFT;

			for (UINT r = 0; r < ElementIndex; ++r)
			{
				if (VertexElements[r].Usage == DeclAddressUsages[UsageType][0] && VertexElements[r].UsageIndex == DeclAddressUsages[UsageType][1])
				{
					VertexElements[ElementIndex].Stream = VertexElements[r].Stream;
					VertexElements[ElementIndex].Offset = VertexElements[r].Offset;
					break;
				}
			}

			VertexElements[ElementIndex].Type = D3DDECLTYPE_FLOAT3;
			VertexElements[ElementIndex].Method = D3DDECLMETHOD_CROSSUV;
			const DWORD Address = (Token & 0xF);
			VertexElements[ElementIndex].Usage = DeclAddressUsages[Address][0];
			VertexElements[ElementIndex].UsageIndex = DeclAddressUsages[Address][1];

			if (VertexElements[ElementIndex].Usage == D3DDECLUSAGE_BLENDINDICES)
			{
				VertexElements[ElementIndex].Method = D3DDECLMETHOD_DEFAULT;
			}

			VertexShaderInputs[ElementIndex++] = Address;
		}
		else if (TokenType == D3DVSD_TOKEN_TESSELLATOR && (Token & 0x10000000))
		{
			VertexElements[ElementIndex].Stream = 0;
			VertexElements[ElementIndex].Offset = 0;
			VertexElements[ElementIndex].Type = D3DDECLTYPE_UNUSED;
			VertexElements[ElementIndex].Method = D3DDECLMETHOD_UV;
			const DWORD Address = (Token & 0xF);
			VertexElements[ElementIndex].Usage = DeclAddressUsages[Address][0];
			VertexElements[ElementIndex].UsageIndex = DeclAddressUsages[Address][1];

			if (VertexElements[ElementIndex].Usage == D3DDECLUSAGE_BLENDINDICES)
			{
				VertexElements[ElementIndex].Method = D3DDECLMETHOD_DEFAULT;
			}

			VertexShaderInputs[ElementIndex++] = Address;
		}
		else if (TokenType == D3DVSD_TOKEN_CONSTMEM)
		{
			const DWORD RegisterCount = 4 * ((Token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT);
			DWORD Address = (Token & D3DVSD_CONSTADDRESSMASK) >> D3DVSD_CONSTADDRESSSHIFT;

			for (DWORD RegisterIndex = 0; RegisterIndex < RegisterCount; RegisterIndex += 4, ++Address)
			{
				ConstantsCode += "    def c" + std::to_string(Address) + ", " +
					std::to_string(*reinterpret_cast<const float *>(&pDeclaration[RegisterIndex + 1])) + ", " +
					std::to_string(*reinterpret_cast<const float *>(&pDeclaration[RegisterIndex + 2])) + ", " +
					std::to_string(*reinterpret_cast<const float *>(&pDeclaration[RegisterIndex + 3])) + ", " +
					std::to_string(*reinterpret_cast<const float *>(&pDeclaration[RegisterIndex + 4])) + " /* vertex declaration constant */\n";
			}

			pDeclaration += RegisterCount;
		}
		else
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed because token type '" << TokenType << "' is not supported!" << std::endl;
#endif

			return D3DERR_INVALIDCALL;
		}

		++pDeclaration;
	}

	const D3DVERTEXELEMENT9 Terminator = D3DDECL_END();
	VertexElements[ElementIndex] = Terminator;

	HRESULT hr;
	VertexShaderInfo *ShaderInfo;

	if (pFunction != nullptr)
	{
#ifndef D3D8TO9NOLOG
		LOG << "> Disassembling shader and translating assembly to Direct3D 9 compatible code ..." << std::endl;
#endif

		if (*pFunction < D3DVS_VERSION(1, 0) || *pFunction > D3DVS_VERSION(1, 1))
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed because of version mismatch ('" << std::showbase << std::hex << *pFunction << std::dec << std::noshowbase << "')! Only 'vs_1_x' shaders are supported." << std::endl;
#endif

			return D3DERR_INVALIDCALL;
		}

		ID3DXBuffer *Disassembly = nullptr, *Assembly = nullptr, *ErrorBuffer = nullptr;

		if (D3DXDisassembleShader != nullptr)
		{
			hr = D3DXDisassembleShader(pFunction, FALSE, nullptr, &Disassembly);
		}
		else
		{
			hr = D3DERR_INVALIDCALL;
		}

		if (FAILED(hr))
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed to disassemble shader with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif

			return hr;
		}

		std::string SourceCode;
		{
			const char* raw = static_cast<const char*>(Disassembly->GetBufferPointer());
			size_t rawSize = Disassembly->GetBufferSize();

			SourceCode.reserve(rawSize);

			for (size_t i = 0; i < rawSize; ++i)
			{
				unsigned char c = static_cast<unsigned char>(raw[i]);

				bool isAllowed =
					(c == '\t') ||
					(c == '\n') ||
					(c == '\r') ||
					(c >= ' ' && c <= '~');

				if (!isAllowed)
					continue;

				SourceCode.push_back(static_cast<char>(c));
			}
		}

#ifndef D3D8TO9NOLOG
		LOG << "> Dumping original shader assembly:" << std::endl << std::endl << SourceCode << std::endl;
#endif

		const size_t VersionPosition = SourceCode.find("vs_1_");

		assert(VersionPosition != std::string::npos);

		if (SourceCode.at(VersionPosition + 5) == '0')
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Replacing version 'vs_1_0' with 'vs_1_1' ..." << std::endl;
#endif

			SourceCode.replace(VersionPosition, 6, "vs_1_1");
		}

		size_t DeclPosition = VersionPosition + 7;

		for (UINT k = 0; k < ElementIndex; k++)
		{
			std::string DeclCode = "    ";

			switch (VertexElements[k].Usage)
			{
			case D3DDECLUSAGE_POSITION:
				DeclCode += "dcl_position";
				break;
			case D3DDECLUSAGE_BLENDWEIGHT:
				DeclCode += "dcl_blendweight";
				break;
			case D3DDECLUSAGE_BLENDINDICES:
				DeclCode += "dcl_blendindices";
				break;
			case D3DDECLUSAGE_NORMAL:
				DeclCode += "dcl_normal";
				break;
			case D3DDECLUSAGE_PSIZE:
				DeclCode += "dcl_psize";
				break;
			case D3DDECLUSAGE_COLOR:
				DeclCode += "dcl_color";
				break;
			case D3DDECLUSAGE_TEXCOORD:
				DeclCode += "dcl_texcoord";
				break;
			}

			if (VertexElements[k].UsageIndex > 0)
			{
				DeclCode += std::to_string(VertexElements[k].UsageIndex);
			}

			DeclCode += " v" + std::to_string(VertexShaderInputs[k]) + '\n';

			SourceCode.insert(DeclPosition, DeclCode);
			DeclPosition += DeclCode.length();
		}

		#pragma region Fill registers with default value
		SourceCode.insert(DeclPosition, ConstantsCode);

		// Get number of arithmetic instructions used
		const size_t InstructionPosition = SourceCode.find("instruction");
		size_t InstructionCount = InstructionPosition > 2 && InstructionPosition < SourceCode.size() ? strtoul(SourceCode.substr(InstructionPosition - 4, 4).c_str(), nullptr, 10) : 0;

		for (size_t j = 0; j < 8; j++)
		{
			const std::string reg = "oT" + std::to_string(j);

			if (SourceCode.find(reg) != std::string::npos && InstructionCount < 128)
			{
				++InstructionCount;
				SourceCode.insert(DeclPosition + ConstantsCode.size(), "    mov " + reg + ", c0 /* initialize output register " + reg + " */\n");
			}
		}
		for (size_t j = 0; j < 2; j++)
		{
			const std::string reg = "oD" + std::to_string(j);

			if (SourceCode.find(reg) != std::string::npos && InstructionCount < 128)
			{
				++InstructionCount;
				SourceCode.insert(DeclPosition + ConstantsCode.size(), "    mov " + reg + ", c0 /* initialize output register " + reg + " */\n");
			}
		}
		for (size_t j = 0; j < 12; j++)
		{
			const std::string reg = "r" + std::to_string(j);

			if (SourceCode.find(reg) != std::string::npos && InstructionCount < 128)
			{
				++InstructionCount;
				SourceCode.insert(DeclPosition + ConstantsCode.size(), "    mov " + reg + ", c0 /* initialize register " + reg + " */\n");
			}
		}
		#pragma endregion

		SourceCode = std::regex_replace(SourceCode, std::regex("    \\/\\/ vs\\.1\\.1\\n((?! ).+\\n)+"), "");
		SourceCode = std::regex_replace(SourceCode, std::regex("([^\\n]\\n)[\\s]*#line [0123456789]+.*\\n"), "$1");
		SourceCode = std::regex_replace(SourceCode, std::regex("(oFog|oPts)\\.x"), "$1 /* removed swizzle */");
		SourceCode = std::regex_replace(SourceCode, std::regex("(add|sub|mul|min|max) (oFog|oPts), ([cr][0-9]+), (.+)\\n"), "$1 $2, $3.x /* added swizzle */, $4\n");
		SourceCode = std::regex_replace(SourceCode, std::regex("(add|sub|mul|min|max) (oFog|oPts), (.+), ([cr][0-9]+)\\n"), "$1 $2, $3, $4.x /* added swizzle */\n");
		SourceCode = std::regex_replace(SourceCode, std::regex("(mov|mad) (oFog|oPts)(.*), (-?)([crv][0-9]+(?![\\.0-9]))"), "$1 $2$3, $4$5.x /* select single component */");

		// Destination register cannot be the same as first source register for m*x* instructions.
		if (std::regex_search(SourceCode, std::regex("m.x.")))
		{
			// Check for unused register
			size_t r;
			for (r = 0; r < 12; r++)
			{
				if (SourceCode.find("r" + std::to_string(r)) == std::string::npos) break;
			}

			// Check if first source register is the same as the destination register
			for (size_t j = 0; j < 12; j++)
			{
				const std::string reg = "(m.x.) (r" + std::to_string(j) + "), ((-?)r" + std::to_string(j) + "([\\.xyzw]*))(?![0-9])";

				while (std::regex_search(SourceCode, std::regex(reg)))
				{
					// If there is enough remaining instructions and an unused register then update to use a temp register
					if (r < 12 && InstructionCount < 128)
					{
						++InstructionCount;
						SourceCode = std::regex_replace(SourceCode, std::regex(reg),
							"mov r" + std::to_string(r) + ", $2 /* added line */\n    $1 $2, $4r" + std::to_string(r) + "$5 /* changed $3 to r" + std::to_string(r) + " */",
							std::regex_constants::format_first_only);
					}
					// Disable line to prevent assembly error
					else
					{
						SourceCode = std::regex_replace(SourceCode, std::regex("(.*" + reg + ".*)"), "/*$1*/ /* disabled this line */");
						break;
					}
				}
			}
		}

		// Vertex shader must minimally write all four components (xyzw) of oPos output register. (fix error X5350)
		if (std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.")) && !std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos,")))
		{
			bool xReg = std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.[y|z|w]*x"));
			bool yReg = std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.[x|z|w]*y"));
			bool zReg = std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.[x|y|w]*z"));
			bool wReg = std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.[x|y|z]*w"));
			if (!xReg || !yReg || !zReg || !wReg)
			{
				SourceCode = std::regex_replace(SourceCode, std::regex("    ([a-z2-4]*) (oPos\\.[x|y|z|w]*,) ([^\\n]*)\\n"), "    $1 oPos, $3 /* removed oPos swizzles */\n");
			}
		}

#ifndef D3D8TO9NOLOG
		LOG << "> Dumping translated shader assembly:" << std::endl << std::endl << SourceCode << std::endl;
#endif

		if (D3DXAssembleShader != nullptr)
		{
			hr = D3DXAssembleShader(SourceCode.data(), static_cast<UINT>(SourceCode.size()), nullptr, nullptr, D3DXASM_FLAGS, &Assembly, &ErrorBuffer);
		}
		else
		{
			hr = D3DERR_INVALIDCALL;
		}

		Disassembly->Release();

		if (FAILED(hr))
		{
			if (ErrorBuffer != nullptr)
			{
#ifndef D3D8TO9NOLOG
				LOG << "> Failed to reassemble shader:" << std::endl << std::endl << static_cast<const char *>(ErrorBuffer->GetBufferPointer()) << std::endl;
#endif
				ErrorBuffer->Release();
			}
			else
			{
#ifndef D3D8TO9NOLOG
				LOG << "> Failed to reassemble shader with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
			}

			return hr;
		}

		ShaderInfo = new VertexShaderInfo();

		hr = ProxyInterface->CreateVertexShader(static_cast<const DWORD *>(Assembly->GetBufferPointer()), &ShaderInfo->Shader);

		Assembly->Release();
	}
	else
	{
		ShaderInfo = new VertexShaderInfo();
		ShaderInfo->Shader = nullptr;

		hr = D3D_OK;
	}

	if (SUCCEEDED(hr))
	{
		hr = ProxyInterface->CreateVertexDeclaration(VertexElements, &ShaderInfo->Declaration);

		if (SUCCEEDED(hr))
		{
			// Since 'Shader' is at least 8 byte aligned, we can safely shift it to right and end up not overwriting the top bit
			assert((reinterpret_cast<DWORD>(ShaderInfo) & 1) == 0);
			const DWORD ShaderMagic = reinterpret_cast<DWORD>(ShaderInfo) >> 1;

			*pHandle = ShaderMagic | 0x80000000;

			VertexShaderHandles.insert(*pHandle);
			VertexShaderAndDeclarationCount++;
			if (ShaderInfo->Shader)
			{
				VertexShaderAndDeclarationCount++;
			}
		}
		else
		{
#ifndef D3D8TO9NOLOG
			LOG << "> 'IDirect3DDevice9::CreateVertexDeclaration' failed with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
			if (ShaderInfo->Shader != nullptr) 
			{
				ShaderInfo->Shader->Release();
			}
		}
	}
	else
	{
#ifndef D3D8TO9NOLOG
		LOG << "> 'IDirect3DDevice9::CreateVertexShader' failed with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
	}

	if (FAILED(hr))
	{
		delete ShaderInfo;
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetVertexShader(DWORD Handle)
{
	HRESULT hr;

	if ((Handle & 0x80000000) == 0)
	{
		ProxyInterface->SetVertexShader(nullptr);
		ProxyInterface->SetVertexDeclaration(nullptr);
		hr = ProxyInterface->SetFVF(Handle);

		CurrentVertexShaderHandle = 0;
	}
	else
	{
		const DWORD handleMagic = Handle << 1;
		VertexShaderInfo *const ShaderInfo = reinterpret_cast<VertexShaderInfo *>(handleMagic);

		hr = ProxyInterface->SetVertexShader(ShaderInfo->Shader);
		ProxyInterface->SetVertexDeclaration(ShaderInfo->Declaration);

		if (SUCCEEDED(hr))
			CurrentVertexShaderHandle = Handle;
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShader(DWORD *pHandle)
{
	if (pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	if (CurrentVertexShaderHandle == 0)
	{
		return ProxyInterface->GetFVF(pHandle);
	}
	else
	{
		*pHandle = CurrentVertexShaderHandle;
		return D3D_OK;
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeleteVertexShader(DWORD Handle)
{
	if ((Handle & 0x80000000) == 0)
		return D3DERR_INVALIDCALL;

	if (VertexShaderHandles.erase(Handle) == 0)
		return D3DERR_INVALIDCALL;

	if (CurrentVertexShaderHandle == Handle)
	{
		ProxyInterface->SetVertexShader(nullptr);
		ProxyInterface->SetVertexDeclaration(nullptr);
		CurrentVertexShaderHandle = 0;
	}

	const DWORD HandleMagic = Handle << 1;
	VertexShaderInfo *const ShaderInfo = reinterpret_cast<VertexShaderInfo *>(HandleMagic);

	if (ShaderInfo->Shader != nullptr) 
	{
		ShaderInfo->Shader->Release();
		VertexShaderAndDeclarationCount--;
	}
	if (ShaderInfo->Declaration != nullptr)
	{
		ShaderInfo->Declaration->Release();
		VertexShaderAndDeclarationCount--;
	}

	delete ShaderInfo;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetVertexShaderConstant(DWORD Register, const void *pConstantData, DWORD ConstantCount)
{
	return ProxyInterface->SetVertexShaderConstantF(Register, static_cast<const float *>(pConstantData), ConstantCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount)
{
	return ProxyInterface->GetVertexShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderDeclaration(DWORD Handle, void *pData, DWORD *pSizeOfData)
{
	UNREFERENCED_PARAMETER(Handle);
	UNREFERENCED_PARAMETER(pData);
	UNREFERENCED_PARAMETER(pSizeOfData);

#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetVertexShaderDeclaration" << "(" << this << ", " << Handle << ", " << pData << ", " << pSizeOfData << ")' ..." << std::endl;
	LOG << "> 'IDirect3DDevice8::GetVertexShaderDeclaration' is not implemented!" << std::endl;
#endif

	return D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetVertexShaderFunction" << "(" << this << ", " << Handle << ", " << pData << ", " << pSizeOfData << ")' ..." << std::endl;
#endif

	if ((Handle & 0x80000000) == 0)
		return D3DERR_INVALIDCALL;

	const DWORD HandleMagic = Handle << 1;
	IDirect3DVertexShader9 *VertexShaderInterface = reinterpret_cast<VertexShaderInfo *>(HandleMagic)->Shader;

	if (VertexShaderInterface == nullptr)
		return D3DERR_INVALIDCALL;

#ifndef D3D8TO9NOLOG
	LOG << "> Returning translated shader byte code." << std::endl;
#endif

	return VertexShaderInterface->GetFunction(pData, reinterpret_cast<UINT *>(pSizeOfData));
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
	IDirect3DVertexBuffer9 *pStreamDataImpl = nullptr;
	if (pStreamData != nullptr)
		pStreamDataImpl = static_cast<Direct3DVertexBuffer8 *>(pStreamData)->GetProxyInterface();
	if (StreamNumber == 0)
	{
		U2Stream0 = static_cast<Direct3DVertexBuffer8 *>(pStreamData);
		U2Stride0 = Stride;
	}

	return ProxyInterface->SetStreamSource(StreamNumber, pStreamDataImpl, 0, Stride);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
	if (ppStreamData == nullptr)
		return D3DERR_INVALIDCALL;

	*ppStreamData = nullptr;

	UINT StreamOffset = 0;
	IDirect3DVertexBuffer9 *VertexBufferInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetStreamSource(StreamNumber, &VertexBufferInterface, &StreamOffset, pStride);
	if (FAILED(hr))
		return hr;

	if (VertexBufferInterface != nullptr)
		*ppStreamData = ProxyAddressLookupTable->FindAddress<Direct3DVertexBuffer8>(VertexBufferInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetIndices(IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
	if (BaseVertexIndex > 0x7FFFFFFF)
		return D3DERR_INVALIDCALL;

	IDirect3DIndexBuffer9 *pIndexDataImpl = nullptr;
	if (pIndexData != nullptr)
		pIndexDataImpl = static_cast<Direct3DIndexBuffer8 *>(pIndexData)->GetProxyInterface();

	const HRESULT hr = ProxyInterface->SetIndices(pIndexDataImpl);
	if (FAILED(hr))
		return hr;
	U2Indices = static_cast<Direct3DIndexBuffer8 *>(pIndexData);

	CurrentBaseVertexIndex = static_cast<INT>(BaseVertexIndex);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetIndices(IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
	if (ppIndexData == nullptr)
		return D3DERR_INVALIDCALL;

	*ppIndexData = nullptr;

	if (pBaseVertexIndex != nullptr)
		*pBaseVertexIndex = static_cast<UINT>(CurrentBaseVertexIndex);

	IDirect3DIndexBuffer9 *IntexBufferInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetIndices(&IntexBufferInterface);
	if (FAILED(hr))
		return hr;

	if (IntexBufferInterface != nullptr)
		*ppIndexData = ProxyAddressLookupTable->FindAddress<Direct3DIndexBuffer8>(IntexBufferInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreatePixelShader(const DWORD *pFunction, DWORD *pHandle)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreatePixelShader" << "(" << this << ", " << pFunction << ", " << pHandle << ")' ..." << std::endl;
#endif

	if (pFunction == nullptr || pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	*pHandle = 0;

#ifndef D3D8TO9NOLOG
	LOG << "> Disassembling shader and translating assembly to Direct3D 9 compatible code ..." << std::endl;
#endif

	if (*pFunction < D3DPS_VERSION(1, 0) || *pFunction > D3DPS_VERSION(1, 4))
	{
#ifndef D3D8TO9NOLOG
		LOG << "> Failed because of version mismatch ('" << std::showbase << std::hex << *pFunction << std::dec << std::noshowbase << "')! Only 'ps_1_x' shaders are supported." << std::endl;
#endif
		return D3DERR_INVALIDCALL;
	}

	ID3DXBuffer *Disassembly = nullptr, *Assembly = nullptr, *ErrorBuffer = nullptr;

	HRESULT hr = D3DERR_INVALIDCALL;

	if (D3DXDisassembleShader != nullptr)
		hr = D3DXDisassembleShader(pFunction, FALSE, nullptr, &Disassembly);

	if (FAILED(hr))
	{
#ifndef D3D8TO9NOLOG
		LOG << "> Failed to disassemble shader with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
		return hr;
	}

	std::string SourceCode;
	{
		const char* raw = static_cast<const char*>(Disassembly->GetBufferPointer());
		size_t rawSize = Disassembly->GetBufferSize();

		SourceCode.reserve(rawSize);

		for (size_t i = 0; i < rawSize; ++i)
		{
			unsigned char c = static_cast<unsigned char>(raw[i]);

			bool isAllowed =
				(c == '\t') ||
				(c == '\n') ||
				(c == '\r') ||
				(c >= ' ' && c <= '~');

			if (!isAllowed)
				continue;

			SourceCode.push_back(static_cast<char>(c));
		}
	}

	const size_t VersionPosition = SourceCode.find("ps_1_");

	assert(VersionPosition != std::string::npos);

	if (SourceCode.at(VersionPosition + 5) == '0')
	{
#ifndef D3D8TO9NOLOG
		LOG << "> Replacing version 'ps_1_0' with 'ps_1_1' ..." << std::endl;
#endif

		SourceCode.replace(VersionPosition, 6, "ps_1_1");
	}

	// Get number of arithmetic instructions used
	const size_t ArithmeticPosition = SourceCode.find("arithmetic");
	size_t ArithmeticCount = ArithmeticPosition > 2 && ArithmeticPosition < SourceCode.size() ? strtoul(SourceCode.substr(ArithmeticPosition - 2, 2).c_str(), nullptr, 10) : 0;
	ArithmeticCount = (ArithmeticCount != 0) ? ArithmeticCount : 10;	// Default to 10

	// Remove lines when "    // ps.1.1" string is found and the next line does not start with a space
	SourceCode = std::regex_replace(SourceCode,
		std::regex("    \\/\\/ ps\\.1\\.[1-4]\\n((?! ).+\\n)+"),
		"");

	// Remove debug lines
	SourceCode = std::regex_replace(SourceCode,
		std::regex("([^\\n]\\n)[\\s]*#line [0123456789]+.*\\n"),
		"$1");

	// Fix '-' modifier for constant values when using 'add' arithmetic by changing it to use 'sub'
	SourceCode = std::regex_replace(SourceCode,
		std::regex("(add)([_satxd248]*) (r[0-9][\\.wxyz]*), ((1-|)[crtv][0-9][\\.wxyz_abdis2]*), (-)(c[0-9][\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]|)(?![_\\.wxyz])"),
		"sub$2 $3, $4, $7$8 /* changed 'add' to 'sub' removed modifier $6 */");

	// Create temporary varables for ps_1_4
	std::string SourceCode14 = SourceCode;
	int ArithmeticCount14 = ArithmeticCount;

	// Fix modifiers for constant values by using any remaining arithmetic places to add an instruction to move the constant value to a temporary register
	while (std::regex_search(SourceCode, std::regex("-c[0-9]|c[0-9][\\.wxyz]*_")) && ArithmeticCount < 8)
	{
		// Make sure that the dest register is not already being used
		const std::string normalizedSourceCode =
			std::regex_replace(
				std::regex_replace(SourceCode,
					std::regex("1?-(c[0-9])[\\._a-z0-9]*"), "-$1"),    // Find negative modifiers
				std::regex("(c[0-9])[\\.wxyz]*_[a-z0-9]*"), "-$1");    // Find swizzle modifiers
		std::string tmpLine = "\n" + normalizedSourceCode + "\n";
		size_t start = tmpLine.substr(0, tmpLine.find("-c")).rfind("\n") + 1;
		tmpLine = tmpLine.substr(start, tmpLine.find("\n", start) - start);
		const std::string destReg = std::regex_replace(tmpLine, std::regex("[ \\+]+[a-z_\\.0-9]+ (r[0-9]).*-c[0-9].*"),"$1");
		const std::string sourceReg = std::regex_replace(tmpLine, std::regex("[ \\+]+[a-z_\\.0-9]+ r[0-9][\\._a-z0-9]*, (.*)-c[0-9](.*)"), "$1$2");
		if (sourceReg.find(destReg) != std::string::npos)
		{
			break;
		}

		// Replace one constant modifier using the dest register as a temporary register
		size_t SourceSize = SourceCode.size();
		SourceCode = std::regex_replace(SourceCode,
			std::regex("    (...)(_[_satxd248]*|) (r[0-9])([\\.wxyz]*), (1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?(1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?(1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?((1?-)(c[0-9])([\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]|)|(1?-?)(c[0-9])([\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]))(?![_\\.wxyz])"),
			"    mov $3$4, $10$11$14$15 /* added line */\n    $1$2 $3$4, $5$6$9$13$3$12$16 /* changed $10$11$14$15 to $3 */", std::regex_constants::format_first_only);
		// Replace one constant modifier on coissued commands using the dest register as a temporary register
		if (SourceSize == SourceCode.size())
		{
			SourceCode = std::regex_replace(SourceCode,
				std::regex("(    .*\\n)  \\+ (...)(_[_satxd248]*|) (r[0-9])([\\.wxyz]*), (1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?(1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?(1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?((1?-)(c[0-9])([\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]|)|(1?-?)(c[0-9])([\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]))(?![_\\.wxyz])"),
				"    mov $4$5, $11$12$15$16 /* added line */\n$1  + $2$3 $4$5, $6$7$10$14$4$13$17 /* changed $11$12$15$16 to $4 */", std::regex_constants::format_first_only);
		}

		if (SourceSize == SourceCode.size())
			break;

		ArithmeticCount++;
	}

	// Check if this should be converted to ps_1_4
	if (std::regex_search(SourceCode, std::regex("-c[0-9]|c[0-9][\\.wxyz]*_")) &&	// Check for modifiers on constants
		!std::regex_search(SourceCode, std::regex("tex[bcdmr]")) &&					// Verify unsupported instructions are not used
		std::regex_search(SourceCode, std::regex("ps_1_[0-3]")))					// Verify PixelShader is using version 1.0 to 1.3
	{
		bool ConvertError = false;
		bool RegisterUsed[7] = { false, false, false, false, false, false, true };

		struct MyStrings
		{
			std::string dest;
			std::string source;
		};

		std::vector<MyStrings> ReplaceReg;
		std::string NewSourceCode = "    ps_1_4 /* converted */\n";

		// Ensure at least one command will be above the phase marker
		bool PhaseMarkerSet = (ArithmeticCount14 >= 8);
		if (SourceCode14.find("def c") == std::string::npos && !PhaseMarkerSet)
		{
			for (size_t j = 0; j < 8; j++)
			{
				const std::string reg = "c" + std::to_string(j);

				if (SourceCode14.find(reg) == std::string::npos)
				{
					PhaseMarkerSet = true;
					NewSourceCode.append("    def " + reg + ", 0, 0, 0, 0 /* added line */\n");
					break;
				}
			}
		}

		// Update registers to use different numbers from textures
		size_t FirstReg = 0;
		for (size_t j = 0; j < 2; j++)
		{
			const std::string reg = "r" + std::to_string(j);

			if (SourceCode14.find(reg) != std::string::npos)
			{
				while (SourceCode14.find("t" + std::to_string(FirstReg)) != std::string::npos ||
					(SourceCode14.find("r" + std::to_string(FirstReg)) != std::string::npos && j != FirstReg))
				{
					FirstReg++;
				}
				SourceCode14 = std::regex_replace(SourceCode14, std::regex(reg), "r" + std::to_string(FirstReg));
				FirstReg++;
			}
		}

		// Set phase location
		size_t PhasePosition = NewSourceCode.length();
		size_t TexturePosition = 0;

		// Loop through each line
		size_t LinePosition = 1;
		std::string NewLine = SourceCode14;
		while (true)
		{
			// Get next line
			size_t tmpLinePos = SourceCode14.find("\n", LinePosition) + 1;
			if (tmpLinePos == std::string::npos || tmpLinePos < LinePosition)
			{
				break;
			}
			LinePosition = tmpLinePos;
			NewLine = SourceCode14.substr(LinePosition, SourceCode14.length());
			tmpLinePos = NewLine.find("\n");
			if (tmpLinePos != std::string::npos)
			{
				NewLine.resize(tmpLinePos);
			}

			// Skip 'ps_x_x' lines
			if (std::regex_search(NewLine, std::regex("ps_._.")))
			{
				// Do nothing
			}

			// Check for 'def' and add before 'phase' statement
			else if (NewLine.find("def c") != std::string::npos)
			{
				PhaseMarkerSet = true;
				const std::string tmpLine = NewLine + "\n";
				NewSourceCode.insert(PhasePosition, tmpLine);
				PhasePosition += tmpLine.length();
			}

			// Check for 'tex' and update to 'texld'
			else if (NewLine.find("tex t") != std::string::npos)
			{
				const std::string regNum = std::regex_replace(NewLine, std::regex(".*tex t([0-9]).*"), "$1");
				const std::string tmpLine = "    texld r" + regNum + ", t" + regNum + "\n";

				// Mark as a texture register and add 'texld' statement before or after the 'phase' statement
				const unsigned long Num = strtoul(regNum.c_str(), nullptr, 10);
				RegisterUsed[(Num < 6) ? Num : 6] = true;
				NewSourceCode.insert(PhasePosition, tmpLine);
				if (PhaseMarkerSet)
				{
					TexturePosition += tmpLine.length();
				}
				else
				{
					PhaseMarkerSet = true;
					PhasePosition += tmpLine.length();
				}
			}

			// Other instructions
			else
			{
				// Check for constant modifiers and update them to use unused temp register
				if (std::regex_search(NewLine, std::regex("-c[0-9]|c[0-9][\\.wxyz]*_")))
				{
					for (size_t j = 0; j < 6; j++)
					{
						std::string reg = "r" + std::to_string(j);

						if (NewSourceCode.find(reg) == std::string::npos)
						{
							const std::string constReg = std::regex_replace(NewLine, std::regex(".*-(c[0-9]).*|.*(c[0-9])[\\.wxyz]*_.*"), "$1$2");

							// Check if this constant has modifiers in more than one line
							if (std::regex_search(SourceCode14.substr(LinePosition + NewLine.length(), SourceCode14.length()), std::regex("-" + constReg + "|" + constReg + "[\\.wxyz]*_")))
							{
								// Find an unused register
								while (j < 6 &&
									(NewSourceCode.find("r" + std::to_string(j)) != std::string::npos ||
									SourceCode14.find("r" + std::to_string(j)) != std::string::npos))
								{
									j++;
								}
								// Replace all constants with the unused register
								if (j < 6)
								{
									reg = "r" + std::to_string(j);
									SourceCode14 = std::regex_replace(SourceCode14, std::regex(constReg), reg);
								}
							}

							const std::string tmpLine = "    mov " + reg + ", " + constReg + "\n";

							// Update the constant in this line and add 'mov' statement before or after the 'phase' statement
							NewLine = std::regex_replace(NewLine, std::regex(constReg), reg);
							if (ArithmeticCount14 < 8)
							{
								NewSourceCode.insert(PhasePosition + TexturePosition, tmpLine);
								ArithmeticCount14++;
							}
							else
							{
								PhaseMarkerSet = true;
								NewSourceCode.insert(PhasePosition, tmpLine);
								PhasePosition += tmpLine.length();
							}
							break;
						}
					}
				}

				// Update register from vector once it is used for the last time
				if (ReplaceReg.size() > 0)
				{
					for (size_t x = 0; x < ReplaceReg.size(); x++)
					{
						// Check if register is used in this line
						if (NewLine.find(ReplaceReg[x].dest) != std::string::npos)
						{
							// Get position of all lines after this line
							size_t start = LinePosition + NewLine.length();
							// Move position to next line if the first line is a co-issed command
							start = (SourceCode14.substr(start, 4).find("+") == std::string::npos) ? start : SourceCode14.find("\n", start + 1);

							// Check if register is used in the code after this position
							if (SourceCode14.find(ReplaceReg[x].dest, start) == std::string::npos)
							{
								// Update dest register using source register from the vector
								NewLine = std::regex_replace(NewLine, std::regex("([ \\+]+[a-z_\\.0-9]+ )r[0-9](.*)"), "$1" + ReplaceReg[x].source + "$2");
								ReplaceReg.erase(ReplaceReg.begin() + x);
								break;
							}
						}
					}
				}

				// Check if texture is no longer being used and update the dest register
				if (std::regex_search(NewLine, std::regex("t[0-9]")))
				{
					const std::string texNum = std::regex_replace(NewLine, std::regex(".*t([0-9]).*"), "$1");

					// Get position of all lines after this line
					size_t start = LinePosition + NewLine.length();
					// Move position to next line if the first line is a co-issed command
					start = (SourceCode14.substr(start, 4).find("+") == std::string::npos) ? start : SourceCode14.find("\n", start + 1);

					// Check if texture is used in the code after this position
					if (SourceCode14.find("t" + texNum, start) == std::string::npos)
					{
						const std::string destRegNum = std::regex_replace(NewLine, std::regex("[ \\+]+[a-z_\\.0-9]+ r([0-9]).*"), "$1");

						// Check if destination register is already being used by a texture register
						const unsigned long Num = strtoul(destRegNum.c_str(), nullptr, 10);
						if (!RegisterUsed[(Num < 6) ? Num : 6])
						{
							// Check if line is using more than one texture and error out
							if (std::regex_search(std::regex_replace(NewLine, std::regex("t" + texNum), "r" + texNum), std::regex("t[0-9]")))
							{
								ConvertError = true;
								break;
							}
							// Check if this is the first or last time the register is used
							if (NewSourceCode.find("r" + destRegNum) == std::string::npos ||
								SourceCode14.find("r" + destRegNum, start) == std::string::npos)
							{
								// Update dest register using texture register
								NewLine = std::regex_replace(NewLine, std::regex("([ \\+]+[a-z_\\.0-9]+ )r[0-9](.*)"), "$1r" + texNum + "$2");
								// Update code replacing all regsiters after the marked position with the texture register
								const std::string tempSourceCode = std::regex_replace(SourceCode14.substr(start, SourceCode14.length()), std::regex("r" + destRegNum), "r" + texNum);
								SourceCode14.resize(start);
								SourceCode14.append(tempSourceCode);
							}
							else
							{
								// If register is still being used then add registers to vector to be replaced later
								RegisterUsed[(Num < 6) ? Num : 6] = true;
								MyStrings tempReplaceReg;
								tempReplaceReg.dest = "r" + destRegNum;
								tempReplaceReg.source = "r" + texNum;
								ReplaceReg.push_back(tempReplaceReg);
							}
						}
					}
				}

				// Add line to SourceCode
				NewLine = std::regex_replace(NewLine, std::regex("t([0-9])"), "r$1") + "\n";
				NewSourceCode.append(NewLine);
			}
		}

		// Add 'phase' instruction
		NewSourceCode.insert(PhasePosition, "    phase\n");

		// If no errors were encountered then check if code assembles
		if (!ConvertError && D3DXAssembleShader != nullptr)
		{
			// Test if ps_1_4 assembles
			if (SUCCEEDED(D3DXAssembleShader(NewSourceCode.data(), static_cast<UINT>(NewSourceCode.size()), nullptr, nullptr, 0, &Assembly, &ErrorBuffer)))
			{
				SourceCode = NewSourceCode;
				Assembly->Release();
				Assembly = nullptr;
			}
			else
			{
#ifndef D3D8TO9NOLOG
				LOG << "> Failed to convert shader to ps_1_4" << std::endl;
				LOG << "> Dumping translated shader assembly:" << std::endl << std::endl << NewSourceCode << std::endl;
#endif
				if (ErrorBuffer != nullptr)
				{
#ifndef D3D8TO9NOLOG
					LOG << "> Failed to reassemble shader:" << std::endl << std::endl << static_cast<const char*>(ErrorBuffer->GetBufferPointer()) << std::endl;
#endif
					ErrorBuffer->Release();
					ErrorBuffer = nullptr;
				}
			}
		}
	}

	// Change '-' modifier for constant values when using 'mad' arithmetic by changing it to use 'sub'
	SourceCode = std::regex_replace(SourceCode,
		std::regex("(mad)([_satxd248]*) (r[0-9][\\.wxyz]*), (1?-?[crtv][0-9][\\.wxyz_abdis2]*), (1?-?[crtv][0-9][\\.wxyz_abdis2]*), (-)(c[0-9][\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]|)(?![_\\.wxyz])"),
		"sub$2 $3, $4, $7$8 /* changed 'mad' to 'sub' removed $5 removed modifier $6 */");

	// Remove trailing modifiers for constant values
	SourceCode = std::regex_replace(SourceCode,
		std::regex("(c[0-9][\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa])"),
		"$1 /* removed modifier $2 */");

	// Remove remaining modifiers for constant values
	SourceCode = std::regex_replace(SourceCode,
		std::regex("(1?-)(c[0-9][\\.wxyz]*(?![\\.wxyz]))"),
		"$2 /* removed modifier $1 */");

#ifndef D3D8TO9NOLOG
	LOG << "> Dumping translated shader assembly:" << std::endl << std::endl << SourceCode << std::endl;
#endif

	if (D3DXAssembleShader != nullptr)
	{
		hr = D3DXAssembleShader(SourceCode.data(), static_cast<UINT>(SourceCode.size()), nullptr, nullptr, D3DXASM_FLAGS, &Assembly, &ErrorBuffer);
	}
	else
	{
		hr = D3DERR_INVALIDCALL;
	}

	Disassembly->Release();

	if (FAILED(hr))
	{
		if (ErrorBuffer != nullptr)
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed to reassemble shader:" << std::endl << std::endl << static_cast<const char *>(ErrorBuffer->GetBufferPointer()) << std::endl;
#endif
			ErrorBuffer->Release();
		}
		else
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed to reassemble shader with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
		}

		return hr;
	}

	hr = ProxyInterface->CreatePixelShader(static_cast<const DWORD *>(Assembly->GetBufferPointer()), reinterpret_cast<IDirect3DPixelShader9 **>(pHandle));

	// (kept for pslog=2, which prints the shader's code)
	const std::string U2Code9(static_cast<const char *>(Assembly->GetBufferPointer()), Assembly->GetBufferSize());
	Assembly->Release();

	if (FAILED(hr))
	{
#ifndef D3D8TO9NOLOG
		LOG << "> 'IDirect3DDevice9::CreatePixelShader' failed with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
	}
	else
	{
		PixelShaderHandles.insert(*pHandle);
		// psreplace=: remember which game shader this is (FNV-1a over the D3D8 tokens)
		DWORD Hash = 2166136261u, n = 0;
		for (const DWORD *t = pFunction; n < 4096; t++, n++)
		{
			Hash = (Hash ^ *t) * 16777619u;
			if (*t == 0x0000FFFF)
				break;
		}
		U2PsHash[*pHandle] = Hash;
		U2.LogGamePS(Hash, n + 1, pFunction[0], U2Code9.data(), U2Code9.size());
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPixelShader(DWORD Handle)
{
	const HRESULT hr = ProxyInterface->SetPixelShader(reinterpret_cast<IDirect3DPixelShader9 *>(Handle));
	if (FAILED(hr))
		return hr;

	CurrentPixelShaderHandle = Handle;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShader(DWORD *pHandle)
{
	if (pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	*pHandle = CurrentPixelShaderHandle;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeletePixelShader(DWORD Handle)
{
	if (Handle == 0)
		return D3DERR_INVALIDCALL;

	if (PixelShaderHandles.erase(Handle) == 0)
		return D3DERR_INVALIDCALL;

	if (CurrentPixelShaderHandle == Handle)
		SetPixelShader(0);

	reinterpret_cast<IDirect3DPixelShader9 *>(Handle)->Release();

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPixelShaderConstant(DWORD Register, const void *pConstantData, DWORD ConstantCount)
{
	return ProxyInterface->SetPixelShaderConstantF(Register, static_cast<const float *>(pConstantData), ConstantCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount)
{
	return ProxyInterface->GetPixelShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetPixelShaderFunction" << "(" << this << ", " << Handle << ", " << pData << ", " << pSizeOfData << ")' ..." << std::endl;
#endif

	if (Handle == 0)
		return D3DERR_INVALIDCALL;

	IDirect3DPixelShader9 *const PixelShaderInterface = reinterpret_cast<IDirect3DPixelShader9 *>(Handle);

#ifndef D3D8TO9NOLOG
	LOG << "> Returning translated shader byte code." << std::endl;
#endif

	return PixelShaderInterface->GetFunction(pData, reinterpret_cast<UINT *>(pSizeOfData));
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawRectPatch(UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo)
{
	return ProxyInterface->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo)
{
	return ProxyInterface->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeletePatch(UINT Handle)
{
	return ProxyInterface->DeletePatch(Handle);
}

void Direct3DDevice8::ApplyClipPlanes()
{
	DWORD index = 0;
	for (const auto plane : StoredClipPlanes)
	{
		if ((ClipPlaneRenderState & (1 << index)) != 0)
			ProxyInterface->SetClipPlane(index, plane);

		index++;
	}
}

void Direct3DDevice8::ReleaseShadersAndStateBlocks()
{
	while (!PixelShaderHandles.empty())
	{
		DWORD Handle = *PixelShaderHandles.begin();
		DeletePixelShader(Handle);
	}

	while (!VertexShaderHandles.empty())
	{
		DWORD Handle = *VertexShaderHandles.begin();
		DeleteVertexShader(Handle);
	}

	VertexShaderAndDeclarationCount = 0;

	while (!StateBlockTokens.empty())
	{
		DWORD Token = *StateBlockTokens.begin();
		DeleteStateBlock(Token);
	}
}
