/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <cstring>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"

using namespace renut::gpu;

namespace {

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x8264D8D0]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x8264D910]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x8264D840]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82276AC0]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x3168 + i * 4)));
  viewport[4] = d.W(0x3178);
  viewport[5] = d.W(0x317C);
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x8264DAE8]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x8264DB38]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x82221930], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x826561F8]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3484));
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x8222F410]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x8222A408]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x8220EE38]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x82205700]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x82222EA0]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x82222F20]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x82223270]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x8227AB68]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x8227AF40]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x8227AEB0]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x8222AA18]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x82658678]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x82220B20]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x8264E2C0]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x82658988]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x8264E1A8]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82287D38]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x8264E128]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82657488] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// Render states, by their index in N&B's table (off_82E506F0).
BANJO_RENUT_HOOK(sub_82171D40, SetRenderStateHook<10>);  // D3DDevice_SetRenderState_ZEnable  // feedingfrenzy2-nx: render state 10 setter (the state tables) (Peggle 1 sub_82434640, N&B sub_8264C140)
BANJO_RENUT_HOOK(sub_82171DB0, SetRenderStateHook<11>);  // D3DDevice_SetRenderState_ZFunc  // feedingfrenzy2-nx: render state 11 setter (the state tables) (Peggle 1 sub_824346B0, N&B sub_821FB7B0)
BANJO_RENUT_HOOK(sub_82171D80, SetRenderStateHook<12>);  // D3DDevice_SetRenderState_ZWriteEnable  // feedingfrenzy2-nx: render state 12 setter (the state tables) (Peggle 1 sub_82434680, N&B sub_8264C180)
BANJO_RENUT_HOOK(sub_82171620, SetRenderStateHook<13>);  // D3DDevice_SetRenderState_FillMode  // feedingfrenzy2-nx: render state 13 setter (the state tables) (Peggle 1 sub_82433ED0, N&B sub_8228F330)
BANJO_RENUT_HOOK(sub_821715F0, SetRenderStateHook<14>);  // D3DDevice_SetRenderState_CullMode  // feedingfrenzy2-nx: render state 14 setter (the state tables) (Peggle 1 sub_82433EA0, N&B sub_8228F350)
BANJO_RENUT_HOOK(sub_82171688, SetRenderStateHook<15>);  // D3DDevice_SetRenderState_AlphaBlendEnable  // feedingfrenzy2-nx: render state 15 setter (the state tables) (Peggle 1 sub_82433FB0, N&B sub_8222A718)
BANJO_RENUT_HOOK(sub_82171A18, SetRenderStateHook<16>);  // D3DDevice_SetRenderState_SeparateAlphaBlendEnable  // feedingfrenzy2-nx: render state 16 setter (the state tables) (Peggle 1 sub_82434340, N&B sub_8222A690)
BANJO_RENUT_HOOK(sub_82171B48, SetRenderStateHook<17>);  // D3DDevice_SetRenderState_BlendFactor  // feedingfrenzy2-nx: render state 17 setter (the state tables) (Peggle 1 sub_82434468, N&B sub_8226E8E0)
BANJO_RENUT_HOOK(sub_821717A8, SetRenderStateHook<18>);  // D3DDevice_SetRenderState_SrcBlend  // feedingfrenzy2-nx: render state 18 setter (the state tables) (Peggle 1 sub_824340D0, N&B sub_82230090)
BANJO_RENUT_HOOK(sub_82171838, SetRenderStateHook<19>);  // D3DDevice_SetRenderState_DestBlend  // feedingfrenzy2-nx: render state 19 setter (the state tables) (Peggle 1 sub_82434160, N&B sub_822307B8)
BANJO_RENUT_HOOK(sub_82171718, SetRenderStateHook<20>);  // D3DDevice_SetRenderState_BlendOp  // feedingfrenzy2-nx: render state 20 setter (the state tables) (Peggle 1 sub_82434040, N&B sub_82226380)
BANJO_RENUT_HOOK(sub_82171938, SetRenderStateHook<21>);  // D3DDevice_SetRenderState_SrcBlendAlpha  // feedingfrenzy2-nx: render state 21 setter (the state tables) (Peggle 1 sub_82434260, N&B sub_822672B8)
BANJO_RENUT_HOOK(sub_821719A8, SetRenderStateHook<22>);  // D3DDevice_SetRenderState_DestBlendAlpha  // feedingfrenzy2-nx: render state 22 setter (the state tables) (Peggle 1 sub_824342D0, N&B sub_822208E8)
BANJO_RENUT_HOOK(sub_821718C8, SetRenderStateHook<23>);  // D3DDevice_SetRenderState_BlendOpAlpha  // feedingfrenzy2-nx: render state 23 setter (the state tables) (Peggle 1 sub_824341F0, N&B sub_8221F5F8)
BANJO_RENUT_HOOK(sub_82171650, SetRenderStateHook<24>);  // D3DDevice_SetRenderState_AlphaTestEnable  // feedingfrenzy2-nx: render state 24 setter (the state tables) (Peggle 1 sub_82433F00, N&B sub_8264BE98)
BANJO_RENUT_HOOK(sub_82171AB0, SetRenderStateHook<25>);  // D3DDevice_SetRenderState_AlphaRef  // feedingfrenzy2-nx: render state 25 setter (the state tables) (Peggle 1 sub_824343D8, N&B sub_8264BF50)
BANJO_RENUT_HOOK(sub_82171B18, SetRenderStateHook<26>);  // D3DDevice_SetRenderState_AlphaFunc  // feedingfrenzy2-nx: render state 26 setter (the state tables) (Peggle 1 sub_82434438, N&B sub_8264BFB8)
BANJO_RENUT_HOOK(sub_82171DE8, SetRenderStateHook<27>);  // D3DDevice_SetRenderState_StencilEnable  // feedingfrenzy2-nx: render state 27 setter (the state tables) (Peggle 1 sub_824346E8, N&B sub_8264C1C0)
BANJO_RENUT_HOOK(sub_82171E28, SetRenderStateHook<28>);  // D3DDevice_SetRenderState_TwoSidedStencilMode  // feedingfrenzy2-nx: render state 28 setter (the state tables) (Peggle 1 sub_82434728, N&B sub_8264C200)
BANJO_RENUT_HOOK(sub_82171E90, SetRenderStateHook<29>);  // D3DDevice_SetRenderState_StencilFail  // feedingfrenzy2-nx: render state 29 setter (the state tables) (Peggle 1 sub_82434790, N&B sub_8264C268)
BANJO_RENUT_HOOK(sub_82171EC8, SetRenderStateHook<30>);  // D3DDevice_SetRenderState_StencilZFail  // feedingfrenzy2-nx: render state 30 setter (the state tables) (Peggle 1 sub_824347C8, N&B sub_8264C2A0)
BANJO_RENUT_HOOK(sub_82171F00, SetRenderStateHook<31>);  // D3DDevice_SetRenderState_StencilPass  // feedingfrenzy2-nx: render state 31 setter (the state tables) (Peggle 1 sub_82434800, N&B sub_8264C2D8)
BANJO_RENUT_HOOK(sub_82171E60, SetRenderStateHook<32>);  // D3DDevice_SetRenderState_StencilFunc  // feedingfrenzy2-nx: render state 32 setter (the state tables) (Peggle 1 sub_82434760, N&B sub_8264C238)
BANJO_RENUT_HOOK(sub_82172000, SetRenderStateHook<33>);  // D3DDevice_SetRenderState_StencilRef  // feedingfrenzy2-nx: render state 33 setter (the state tables) (Peggle 1 sub_82434900, N&B sub_8264C3D8)
BANJO_RENUT_HOOK(sub_82172020, SetRenderStateHook<34>);  // D3DDevice_SetRenderState_StencilMask  // feedingfrenzy2-nx: render state 34 setter (the state tables) (Peggle 1 sub_82434920, N&B sub_8264C3F8)
BANJO_RENUT_HOOK(sub_82172040, SetRenderStateHook<35>);  // D3DDevice_SetRenderState_StencilWriteMask  // feedingfrenzy2-nx: render state 35 setter (the state tables) (Peggle 1 sub_82434940, N&B sub_8264C418)
BANJO_RENUT_HOOK(sub_82171F60, SetRenderStateHook<36>);  // D3DDevice_SetRenderState_CCWStencilFail  // feedingfrenzy2-nx: render state 36 setter (the state tables) (Peggle 1 sub_82434860, N&B sub_8264C338)
BANJO_RENUT_HOOK(sub_82171F98, SetRenderStateHook<37>);  // D3DDevice_SetRenderState_CCWStencilZFail  // feedingfrenzy2-nx: render state 37 setter (the state tables) (Peggle 1 sub_82434898, N&B sub_8264C370)
BANJO_RENUT_HOOK(sub_82171FD0, SetRenderStateHook<38>);  // D3DDevice_SetRenderState_CCWStencilPass  // feedingfrenzy2-nx: render state 38 setter (the state tables) (Peggle 1 sub_824348D0, N&B sub_8264C3A8)
BANJO_RENUT_HOOK(sub_82171F30, SetRenderStateHook<39>);  // D3DDevice_SetRenderState_CCWStencilFunc  // feedingfrenzy2-nx: render state 39 setter (the state tables) (Peggle 1 sub_82434830, N&B sub_8264C308)
BANJO_RENUT_HOOK(sub_82172060, SetRenderStateHook<40>);  // D3DDevice_SetRenderState_CCWStencilRef  // feedingfrenzy2-nx: render state 40 setter (the state tables) (Peggle 1 sub_82434960, N&B sub_8264C438)
BANJO_RENUT_HOOK(sub_82172080, SetRenderStateHook<41>);  // D3DDevice_SetRenderState_CCWStencilMask  // feedingfrenzy2-nx: render state 41 setter (the state tables) (Peggle 1 sub_82434980, N&B sub_8264C458)
BANJO_RENUT_HOOK(sub_821720A0, SetRenderStateHook<42>);  // D3DDevice_SetRenderState_CCWStencilWriteMask  // feedingfrenzy2-nx: render state 42 setter (the state tables) (Peggle 1 sub_824349A0, N&B sub_8264C478)
BANJO_RENUT_HOOK(sub_821720C0, SetRenderStateHook<43>);  // D3DDevice_SetRenderState_ClipPlaneEnable  // feedingfrenzy2-nx: render state 43 setter (the state tables) (Peggle 1 sub_824349C0, N&B sub_8228F370)
BANJO_RENUT_HOOK(sub_821723E8, SetRenderStateHook<44>);  // D3DDevice_SetRenderState_PointSize  // feedingfrenzy2-nx: render state 44 setter (the state tables) (Peggle 1 sub_82434CE8, N&B sub_8264C6C0)
BANJO_RENUT_HOOK(sub_82172448, SetRenderStateHook<45>);  // D3DDevice_SetRenderState_PointSizeMin  // feedingfrenzy2-nx: render state 45 setter (the state tables) (Peggle 1 sub_82434D40, N&B sub_8264C720)
BANJO_RENUT_HOOK(sub_821723D8, SetRenderStateHook<46>);  // D3DDevice_SetRenderState_PointSpriteEnable  // feedingfrenzy2-nx: render state 46 setter (the state tables) (Peggle 1 sub_82434CD8, N&B sub_8264C6B0)
BANJO_RENUT_HOOK(sub_821724A0, SetRenderStateHook<47>);  // D3DDevice_SetRenderState_PointSizeMax  // feedingfrenzy2-nx: render state 47 setter (the state tables) (Peggle 1 sub_82434D90, N&B sub_822687D0)
BANJO_RENUT_HOOK(sub_82172288, SetRenderStateHook<48>);  // D3DDevice_SetRenderState_MultiSampleAntiAlias  // feedingfrenzy2-nx: render state 48 setter (the state tables) (Peggle 1 sub_82434B88, N&B sub_8264C598)
BANJO_RENUT_HOOK(sub_821722B8, SetRenderStateHook<49>);  // D3DDevice_SetRenderState_MultiSampleMask  // feedingfrenzy2-nx: render state 49 setter (the state tables) (Peggle 1 sub_82434BB8, N&B sub_8264C5C8)
BANJO_RENUT_HOOK(sub_82174150, SetRenderStateHook<50>);  // D3DDevice_SetRenderState_ScissorTestEnable  // feedingfrenzy2-nx: render state 50 setter (the state tables) (Peggle 1 sub_82436FF8, N&B sub_8264C4A8)
BANJO_RENUT_HOOK(sub_82172118, SetRenderStateHook<51>);  // D3DDevice_SetRenderState_SlopeScaleDepthBias  // feedingfrenzy2-nx: render state 51 setter (the state tables) (Peggle 1 sub_82434A18, N&B sub_8264C4C0)
BANJO_RENUT_HOOK(sub_821721E0, SetRenderStateHook<52>);  // D3DDevice_SetRenderState_DepthBias  // feedingfrenzy2-nx: render state 52 setter (the state tables) (Peggle 1 sub_82434AE0, N&B sub_821FB718)
BANJO_RENUT_HOOK(sub_821722D8, SetRenderStateHook<53>);  // D3DDevice_SetRenderState_ColorWriteEnable  // feedingfrenzy2-nx: render state 53 setter (the state tables) (Peggle 1 sub_82434BD8, N&B sub_821FB6E0)
BANJO_RENUT_HOOK(sub_82172318, SetRenderStateHook<54>);  // D3DDevice_SetRenderState_ColorWriteEnable1  // feedingfrenzy2-nx: render state 54 setter (the state tables) (Peggle 1 sub_82434C18, N&B sub_8264C5F0)
BANJO_RENUT_HOOK(sub_82172358, SetRenderStateHook<55>);  // D3DDevice_SetRenderState_ColorWriteEnable2  // feedingfrenzy2-nx: render state 55 setter (the state tables) (Peggle 1 sub_82434C58, N&B sub_8264C630)
BANJO_RENUT_HOOK(sub_82172398, SetRenderStateHook<56>);  // D3DDevice_SetRenderState_ColorWriteEnable3  // feedingfrenzy2-nx: render state 56 setter (the state tables) (Peggle 1 sub_82434C98, N&B sub_8264C670)
BANJO_RENUT_HOOK(sub_82172BD8, SetRenderStateHook<57>);  // D3DDevice_SetRenderState_TessellationMode  // feedingfrenzy2-nx: render state 57 setter (the state tables) (Peggle 1 sub_824355C0, N&B sub_8264CDC0)
BANJO_RENUT_HOOK(sub_82172B68, SetRenderStateHook<58>);  // D3DDevice_SetRenderState_MinTessellationLevel  // feedingfrenzy2-nx: render state 58 setter (the state tables) (Peggle 1 sub_82435550, N&B sub_8264CD50)
BANJO_RENUT_HOOK(sub_82172BA0, SetRenderStateHook<59>);  // D3DDevice_SetRenderState_MaxTessellationLevel  // feedingfrenzy2-nx: render state 59 setter (the state tables) (Peggle 1 sub_82435588, N&B sub_8264CD88)
BANJO_RENUT_HOOK(sub_821724F8, SetRenderStateHook<60>);  // D3DDevice_SetRenderState_Wrap0  // feedingfrenzy2-nx: render state 60 setter (the state tables) (Peggle 1 sub_82434E60, N&B sub_8264C780)
BANJO_RENUT_HOOK(sub_82172518, SetRenderStateHook<61>);  // D3DDevice_SetRenderState_Wrap1  // feedingfrenzy2-nx: render state 61 setter (the state tables) (Peggle 1 sub_82434E80, N&B sub_8264C7A0)
BANJO_RENUT_HOOK(sub_82172540, SetRenderStateHook<62>);  // D3DDevice_SetRenderState_Wrap2  // feedingfrenzy2-nx: render state 62 setter (the state tables) (Peggle 1 sub_82434EA8, N&B sub_8264C7C8)
BANJO_RENUT_HOOK(sub_82172568, SetRenderStateHook<63>);  // D3DDevice_SetRenderState_Wrap3  // feedingfrenzy2-nx: render state 63 setter (the state tables) (Peggle 1 sub_82434ED0, N&B sub_8264C7F0)
BANJO_RENUT_HOOK(sub_82172590, SetRenderStateHook<64>);  // D3DDevice_SetRenderState_Wrap4  // feedingfrenzy2-nx: render state 64 setter (the state tables) (Peggle 1 sub_82434EF8, N&B sub_8264C818)
BANJO_RENUT_HOOK(sub_821725B8, SetRenderStateHook<65>);  // D3DDevice_SetRenderState_Wrap5  // feedingfrenzy2-nx: render state 65 setter (the state tables) (Peggle 1 sub_82434F20, N&B sub_8264C840)
BANJO_RENUT_HOOK(sub_821725E0, SetRenderStateHook<66>);  // D3DDevice_SetRenderState_Wrap6  // feedingfrenzy2-nx: render state 66 setter (the state tables) (Peggle 1 sub_82434F48, N&B sub_8264C868)
BANJO_RENUT_HOOK(sub_82172608, SetRenderStateHook<67>);  // D3DDevice_SetRenderState_Wrap7  // feedingfrenzy2-nx: render state 67 setter (the state tables) (Peggle 1 sub_82434F70, N&B sub_8264C890)
BANJO_RENUT_HOOK(sub_82172628, SetRenderStateHook<68>);  // D3DDevice_SetRenderState_Wrap8  // feedingfrenzy2-nx: render state 68 setter (the state tables) (Peggle 1 sub_82434F90, N&B sub_8264C8B0)
BANJO_RENUT_HOOK(sub_82172648, SetRenderStateHook<69>);  // D3DDevice_SetRenderState_Wrap9  // feedingfrenzy2-nx: render state 69 setter (the state tables) (Peggle 1 sub_82434FB0, N&B sub_8264C8D0)
BANJO_RENUT_HOOK(sub_82172670, SetRenderStateHook<70>);  // D3DDevice_SetRenderState_Wrap10  // feedingfrenzy2-nx: render state 70 setter (the state tables) (Peggle 1 sub_82434FD8, N&B sub_8264C8F8)
BANJO_RENUT_HOOK(sub_82172698, SetRenderStateHook<71>);  // D3DDevice_SetRenderState_Wrap11  // feedingfrenzy2-nx: render state 71 setter (the state tables) (Peggle 1 sub_82435000, N&B sub_8264C920)
BANJO_RENUT_HOOK(sub_821726C0, SetRenderStateHook<72>);  // D3DDevice_SetRenderState_Wrap12  // feedingfrenzy2-nx: render state 72 setter (the state tables) (Peggle 1 sub_82435028, N&B sub_8264C948)
BANJO_RENUT_HOOK(sub_821726E8, SetRenderStateHook<73>);  // D3DDevice_SetRenderState_Wrap13  // feedingfrenzy2-nx: render state 73 setter (the state tables) (Peggle 1 sub_82435050, N&B sub_8264C970)
BANJO_RENUT_HOOK(sub_82172710, SetRenderStateHook<74>);  // D3DDevice_SetRenderState_Wrap14  // feedingfrenzy2-nx: render state 74 setter (the state tables) (Peggle 1 sub_82435078, N&B sub_8264C998)
BANJO_RENUT_HOOK(sub_82172738, SetRenderStateHook<75>);  // D3DDevice_SetRenderState_Wrap15  // feedingfrenzy2-nx: render state 75 setter (the state tables) (Peggle 1 sub_824350A0, N&B sub_8264C9C0)
BANJO_RENUT_HOOK(sub_82172858, SetRenderStateHook<76>);  // D3DDevice_SetRenderState_ViewportEnable  // feedingfrenzy2-nx: render state 76 setter (the state tables) (Peggle 1 sub_824351C0, N&B sub_82205A80)
BANJO_RENUT_HOOK(sub_821728A8, SetRenderStateHook<77>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable  // feedingfrenzy2-nx: render state 77 setter (the state tables) (Peggle 1 sub_824352F0, N&B sub_8264CAF0)
BANJO_RENUT_HOOK(sub_82172958, SetRenderStateHook<78>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable1  // feedingfrenzy2-nx: render state 78 setter (the state tables) (Peggle 1 sub_82435388, N&B sub_8264CB88)
BANJO_RENUT_HOOK(sub_82172A08, SetRenderStateHook<79>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable2  // feedingfrenzy2-nx: render state 79 setter (the state tables) (Peggle 1 sub_82435420, N&B sub_8264CC20)
BANJO_RENUT_HOOK(sub_82172AB8, SetRenderStateHook<80>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable3  // feedingfrenzy2-nx: render state 80 setter (the state tables) (Peggle 1 sub_824354B8, N&B sub_8264CCB8)
BANJO_RENUT_HOOK(sub_82172C10, SetRenderStateHook<81>);  // D3DDevice_SetRenderState_HalfPixelOffset  // feedingfrenzy2-nx: render state 81 setter (the state tables) (Peggle 1 sub_824355F8, N&B sub_8264CDF8)
BANJO_RENUT_HOOK(sub_82172C48, SetRenderStateHook<82>);  // D3DDevice_SetRenderState_PrimitiveResetEnable  // feedingfrenzy2-nx: render state 82 setter (the state tables) (Peggle 1 sub_82435630, N&B sub_8264CE30)
BANJO_RENUT_HOOK(sub_82172C78, SetRenderStateHook<83>);  // D3DDevice_SetRenderState_PrimitiveResetIndex  // feedingfrenzy2-nx: render state 83 setter (the state tables) (Peggle 1 sub_82435660, N&B sub_8264CE60)
BANJO_RENUT_HOOK(sub_82172CA0, SetRenderStateHook<84>);  // D3DDevice_SetRenderState_AlphaToMaskEnable  // feedingfrenzy2-nx: render state 84 setter (the state tables) (Peggle 1 sub_82435688, N&B sub_8264CE88)
BANJO_RENUT_HOOK(sub_82172CD0, SetRenderStateHook<85>);  // D3DDevice_SetRenderState_AlphaToMaskOffsets  // feedingfrenzy2-nx: render state 85 setter (the state tables) (Peggle 1 sub_824356B8, N&B sub_8264CEB8)
BANJO_RENUT_HOOK(sub_82172D08, SetRenderStateHook<86>);  // D3DDevice_SetRenderState_GuardBand_X  // feedingfrenzy2-nx: render state 86 setter (the state tables) (Peggle 1 sub_824356F0, N&B sub_8264CEF0)
BANJO_RENUT_HOOK(sub_82172D38, SetRenderStateHook<87>);  // D3DDevice_SetRenderState_GuardBand_Y  // feedingfrenzy2-nx: render state 87 setter (the state tables) (Peggle 1 sub_82435720, N&B sub_8264CF20)
BANJO_RENUT_HOOK(sub_82172D68, SetRenderStateHook<88>);  // D3DDevice_SetRenderState_DiscardBand_X  // feedingfrenzy2-nx: render state 88 setter (the state tables) (Peggle 1 sub_82435750, N&B sub_8264CF50)
BANJO_RENUT_HOOK(sub_82172D90, SetRenderStateHook<89>);  // D3DDevice_SetRenderState_DiscardBand_Y  // feedingfrenzy2-nx: render state 89 setter (the state tables) (Peggle 1 sub_82435778, N&B sub_8264CF78)
BANJO_RENUT_HOOK(sub_82172E58, SetRenderStateHook<90>);  // D3DDevice_SetRenderState_HiStencilEnable  // feedingfrenzy2-nx: render state 90 setter (the state tables) (Peggle 1 sub_82435840, N&B sub_8264CFE8)
BANJO_RENUT_HOOK(sub_82172E88, SetRenderStateHook<91>);  // D3DDevice_SetRenderState_HiStencilWriteEnable  // feedingfrenzy2-nx: render state 91 setter (the state tables) (Peggle 1 sub_82435870, N&B sub_8264D018)
BANJO_RENUT_HOOK(sub_82172EB8, SetRenderStateHook<92>);  // D3DDevice_SetRenderState_HiStencilFunc  // feedingfrenzy2-nx: render state 92 setter (the state tables) (Peggle 1 sub_824358A0, N&B sub_8264D048)
BANJO_RENUT_HOOK(sub_82172EE8, SetRenderStateHook<93>);  // D3DDevice_SetRenderState_HiStencilRef  // feedingfrenzy2-nx: render state 93 setter (the state tables) (Peggle 1 sub_824358D0, N&B sub_8264D078)
BANJO_RENUT_HOOK(sub_82172F08, SetRenderStateHook<94>);  // D3DDevice_SetRenderState_PresentInterval  // feedingfrenzy2-nx: render state 94 setter (the state tables) (Peggle 1 sub_824358F0, N&B sub_8264D098)
BANJO_RENUT_HOOK(sub_82172F18, SetRenderStateHook<95>);  // D3DDevice_SetRenderState_PresentImmediateThreshold  // feedingfrenzy2-nx: render state 95 setter (the state tables) (Peggle 1 sub_82435900, N&B sub_8264D0A8)
BANJO_RENUT_HOOK(sub_82172DC0, SetRenderStateHook<96>);  // D3DDevice_SetRenderState_HiZEnable  // feedingfrenzy2-nx: render state 96 setter (the state tables) (Peggle 1 sub_824357A8, N&B sub_8264CFA8)
BANJO_RENUT_HOOK(sub_82172DF0, SetRenderStateHook<97>);  // D3DDevice_SetRenderState_HiZWriteEnable  // feedingfrenzy2-nx: render state 97 setter (the state tables) (Peggle 1 sub_824357D8, N&B sub_822899D0)
BANJO_RENUT_HOOK(sub_82171C68, SetRenderStateHook<98>);  // D3DDevice_SetRenderState_LastPixel  // feedingfrenzy2-nx: render state 98 setter (the state tables) (Peggle 1 sub_82434568, N&B sub_8264C060)
BANJO_RENUT_HOOK(sub_82171CA0, SetRenderStateHook<99>);  // D3DDevice_SetRenderState_LineWidth  // feedingfrenzy2-nx: render state 99 setter (the state tables) (Peggle 1 sub_824345A0, N&B sub_8264C098)
BANJO_RENUT_HOOK(sub_82171D10, SetRenderStateHook<100>);  // D3DDevice_SetRenderState_Buffer2Frames  // feedingfrenzy2-nx: render state 100 setter (the state tables) (Peggle 1 sub_82434610, N&B sub_8264C110)

// Sampler states, by their index in N&B's table (off_82E50BD0).
BANJO_RENUT_HOOK(sub_82173678, SetSamplerStateHook<0>);  // D3DDevice_SetSamplerState_AddressU  // feedingfrenzy2-nx: sampler state 0 setter (the state tables) (Peggle 1 sub_82436098, N&B sub_8227D2D0)
BANJO_RENUT_HOOK(sub_821736C8, SetSamplerStateHook<1>);  // D3DDevice_SetSamplerState_AddressV  // feedingfrenzy2-nx: sampler state 1 setter (the state tables) (Peggle 1 sub_824360E8, N&B sub_8220C4E0)
BANJO_RENUT_HOOK(sub_82173718, SetSamplerStateHook<2>);  // D3DDevice_SetSamplerState_AddressW  // feedingfrenzy2-nx: sampler state 2 setter (the state tables) (Peggle 1 sub_82436138, N&B sub_8264D630)
BANJO_RENUT_HOOK(sub_82173608, SetSamplerStateHook<3>);  // D3DDevice_SetSamplerState_BorderColor  // feedingfrenzy2-nx: sampler state 3 setter (the state tables) (Peggle 1 sub_82436028, N&B sub_8264D590)
BANJO_RENUT_HOOK(sub_821730C8, SetSamplerStateHook<4>);  // D3DDevice_SetSamplerState_MagFilter  // feedingfrenzy2-nx: sampler state 4 setter (the state tables) (Peggle 1 sub_82435AC8, N&B sub_8222F1B8)
BANJO_RENUT_HOOK(sub_82172F38, SetSamplerStateHook<5>);  // D3DDevice_SetSamplerState_MinFilter  // feedingfrenzy2-nx: sampler state 5 setter (the state tables) (Peggle 1 sub_82435920, N&B sub_8222F280)
BANJO_RENUT_HOOK(sub_82173258, SetSamplerStateHook<6>);  // D3DDevice_SetSamplerState_MipFilter  // feedingfrenzy2-nx: sampler state 6 setter (the state tables) (Peggle 1 sub_82435C68, N&B sub_8228F2F0)
BANJO_RENUT_HOOK(sub_82173468, SetSamplerStateHook<7>);  // D3DDevice_SetSamplerState_MipMapLodBias  // feedingfrenzy2-nx: sampler state 7 setter (the state tables) (Peggle 1 sub_82435E88, N&B sub_8264D460)
BANJO_RENUT_HOOK(sub_82173508, SetSamplerStateHook<8>);  // D3DDevice_SetSamplerState_MaxMipLevel  // feedingfrenzy2-nx: sampler state 8 setter (the state tables) (Peggle 1 sub_82435F28, N&B sub_8222F590)
BANJO_RENUT_HOOK(sub_82173348, SetSamplerStateHook<9>);  // D3DDevice_SetSamplerState_MaxAnisotropy  // feedingfrenzy2-nx: sampler state 9 setter (the state tables) (Peggle 1 sub_82435D68, N&B sub_8264D340)
BANJO_RENUT_HOOK(sub_821731C0, SetSamplerStateHook<10>);  // D3DDevice_SetSamplerState_MagFilterZ  // feedingfrenzy2-nx: sampler state 10 setter (the state tables) (Peggle 1 sub_82435BC8, N&B sub_8264D1E0)
BANJO_RENUT_HOOK(sub_82173030, SetSamplerStateHook<11>);  // D3DDevice_SetSamplerState_MinFilterZ  // feedingfrenzy2-nx: sampler state 11 setter (the state tables) (Peggle 1 sub_82435A20, N&B sub_8264D100)
BANJO_RENUT_HOOK(sub_821732B0, SetSamplerStateHook<12>);  // D3DDevice_SetSamplerState_SeparateZFilterEnable  // feedingfrenzy2-nx: sampler state 12 setter (the state tables) (Peggle 1 sub_82435CC0, N&B sub_8264D298)
BANJO_RENUT_HOOK(sub_82173588, SetSamplerStateHook<13>);  // D3DDevice_SetSamplerState_MinMipLevel  // feedingfrenzy2-nx: sampler state 13 setter (the state tables) (Peggle 1 sub_82435FA8, N&B sub_8264D510)
BANJO_RENUT_HOOK(sub_82173768, SetSamplerStateHook<14>);  // D3DDevice_SetSamplerState_TrilinearThreshold  // feedingfrenzy2-nx: sampler state 14 setter (the state tables) (Peggle 1 sub_82436188, N&B sub_8264D680)
BANJO_RENUT_HOOK(sub_821733C0, SetSamplerStateHook<15>);  // D3DDevice_SetSamplerState_AnisotropyBias  // feedingfrenzy2-nx: sampler state 15 setter (the state tables) (Peggle 1 sub_82435DE0, N&B sub_8264D3B8)
BANJO_RENUT_HOOK(sub_821737C0, SetSamplerStateHook<16>);  // D3DDevice_SetSamplerState_HGradientExpBias  // feedingfrenzy2-nx: sampler state 16 setter (the state tables) (Peggle 1 sub_824361E0, N&B sub_8264D6D8)
BANJO_RENUT_HOOK(sub_82173818, SetSamplerStateHook<17>);  // D3DDevice_SetSamplerState_VGradientExpBias  // feedingfrenzy2-nx: sampler state 17 setter (the state tables) (Peggle 1 sub_82436238, N&B sub_8264D730)
BANJO_RENUT_HOOK(sub_82173870, SetSamplerStateHook<18>);  // D3DDevice_SetSamplerState_WhiteBorderColorW  // feedingfrenzy2-nx: sampler state 18 setter (the state tables) (Peggle 1 sub_82436290, N&B sub_8264D788)
BANJO_RENUT_HOOK(sub_821738C8, SetSamplerStateHook<19>);  // D3DDevice_SetSamplerState_PointBorderEnable  // feedingfrenzy2-nx: sampler state 19 setter (the state tables) (Peggle 1 sub_824362E8, N&B sub_8264D7E0)

BANJO_RENUT_HOOK(sub_82169148, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture  // feedingfrenzy2-nx: sig 0.97; trace (dev, sampler, texture, mask) (Peggle 1 sub_82429170, N&B sub_8222F410)
#if 0  // peggle2-nx / peggle1-nx: N&B sub_82B96538 (D3DDevice_SetTexture_Inline) is linked in neither Peggle 2 nor Peggle 1
BANJO_RENUT_HOOK(sub_82B96538, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture_Inline
#endif
BANJO_RENUT_HOOK(sub_82173A28, D3DDevice_SetStreamSource_hook);  // D3DDevice_SetStreamSource  // feedingfrenzy2-nx: sig 0.97; trace (dev, 0, vb, offset, stride 0x24) (Peggle 1 sub_824364D0, N&B sub_8222A408)
BANJO_RENUT_HOOK(sub_82173B48, D3DDevice_SetIndices_hook);  // D3DDevice_SetIndices  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_82436678, N&B sub_8220EE38)
BANJO_RENUT_HOOK(sub_8216B570, D3DDevice_SetVertexDeclaration_hook);  // D3DDevice_SetVertexDeclaration  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_8242CCD8, N&B sub_82205700)
BANJO_RENUT_HOOK(sub_82174160, D3DDevice_SetViewport_hook);  // D3DDevice_SetViewport  // feedingfrenzy2-nx: sig 0.99 (Peggle 1 sub_82437008, N&B sub_82222EA0)
BANJO_RENUT_HOOK(sub_82173EE8, D3D_SetViewport_hook);  // D3D_SetViewport  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_82436DA0, N&B sub_82222F20)
BANJO_RENUT_HOOK(sub_82173928, D3DDevice_SetScissorRect_hook);  // D3DDevice_SetScissorRect  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_82436348, N&B sub_82223178)
BANJO_RENUT_HOOK(sub_82171310, D3D_SetSurfaceClip_hook);  // D3D_SetSurfaceClip  // feedingfrenzy2-nx: sig 0.99 (Peggle 1 sub_82433BB0, N&B sub_82223270)
BANJO_RENUT_HOOK(sub_82174298, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget  // feedingfrenzy2-nx: sig 0.94 (Peggle 1 sub_82437138, N&B sub_8227AB68)
BANJO_RENUT_HOOK(sub_82174600, D3DDevice_SetDepthStencilSurface_hook);  // D3DDevice_SetDepthStencilSurface  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_82437480, N&B sub_8227AF40)
BANJO_RENUT_HOOK(sub_82174200, D3D_SetSurfaceInfo_hook);  // D3D_SetSurfaceInfo  // feedingfrenzy2-nx: sig 0.85; reads the full viewport 0x82018DD0 (Peggle 1 sub_824370A8, N&B sub_8227AEB0)
BANJO_RENUT_HOOK(sub_82182240, D3DDevice_SetPredication_hook);  // D3DDevice_SetPredication  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_82439910, N&B sub_8222AA18)
// feedingfrenzy2-nx: not linked or not called in XDK 6534: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetCommandBufferPredication_hook);  // D3DDevice_SetCommandBufferPredication  // Peggle 1 sub_82446398, N&B sub_82658678
BANJO_RENUT_HOOK(sub_8216B700, D3DDevice_SetShaderGPRAllocation_hook);  // D3DDevice_SetShaderGPRAllocation  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_8242D220, N&B sub_82220B20)
BANJO_RENUT_HOOK(sub_8216AAA8, D3D_CleanupGprAllocationSetup_hook);  // D3D_CleanupGprAllocationSetup  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_8242BD68, N&B sub_8264E2C0)
BANJO_RENUT_HOOK(sub_8217B010, D3DDevice_SetScreenExtentQueryMode_hook);  // D3DDevice_SetScreenExtentQueryMode  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_8244B258, N&B sub_82658988)
BANJO_RENUT_HOOK(sub_8217D368, D3DDevice_UnsetAll_hook);  // D3DDevice_UnsetAll  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_82444060, N&B sub_8264E1A8)
BANJO_RENUT_HOOK(sub_82173DF0, D3D_ResetAllState_hook);  // D3D_ResetAllState  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_82436CA8, N&B sub_82287D38)
// feedingfrenzy2-nx: not linked or not called in XDK 6534: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3D_InvalidateConstants_hook);  // D3D_InvalidateConstants  // Peggle 1 sub_82443FC8, N&B sub_8264E128
BANJO_RENUT_HOOK(sub_82176E10, D3DDevice_BlockUntilIdle_hook);  // D3DDevice_BlockUntilIdle  // feedingfrenzy2-nx: sig 0.99 (Peggle 1 sub_82440610, N&B sub_82657488)
// Through a thunk (j_D3DDevice_SetRenderTarget 0x8264D8C8).
BANJO_RENUT_HOOK(sub_821748D8, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget_Thunk  // feedingfrenzy2-nx: `b 0x82174298`; trace (dev, 0, surface) (Peggle 1 sub_82437D68, N&B sub_8264D8C8)
BANJO_RENUT_HOOK(sub_82173BD8, D3DDevice_GetRenderTarget_hook);  // D3DDevice_GetRenderTarget  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_82436748, N&B sub_8264D8D0)
BANJO_RENUT_HOOK(sub_82173C20, D3DDevice_GetDepthStencilSurface_hook);  // D3DDevice_GetDepthStencilSurface  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_82436790, N&B sub_8264D910)
// feedingfrenzy2-nx: not linked or not called in XDK 6534: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_GetStreamSource_hook);  // D3DDevice_GetStreamSource  // Peggle 1 sub_824365F0, N&B sub_8264D840
// feedingfrenzy2-nx: not linked or not called in XDK 6534: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_GetViewport_hook);  // D3DDevice_GetViewport  // Peggle 1 sub_82436468, N&B sub_82276AC0
// feedingfrenzy2-nx: not linked or not called in XDK 6534: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetClipPlane_hook);  // D3DDevice_SetClipPlane  // Peggle 1 sub_82436A20, N&B sub_8264DAE8
// feedingfrenzy2-nx: not linked or not called in XDK 6534: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_SetVertexFetchConstant_hook);  // D3DDevice_SetVertexFetchConstant  // Peggle 1 sub_82436B20, N&B sub_8264DB38
BANJO_RENUT_HOOK(sub_8216AF28, D3DDevice_SetVertexShaderConstantB_hook);  // D3DDevice_SetVertexShaderConstantB  // feedingfrenzy2-nx: sig 1.00 (before the pixel one, as in Peggle 1) (Peggle 1 sub_8242C450, N&B sub_82221930)
BANJO_RENUT_HOOK(sub_8216AF88, D3DDevice_SetPixelShaderConstantB_hook);  // D3DDevice_SetPixelShaderConstantB  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_8242C4B0, N&B sub_82268810)
BANJO_RENUT_HOOK(sub_8216AFE8, D3DDevice_SetVertexShaderConstantI_hook);  // D3DDevice_SetVertexShaderConstantI  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_8242C5A0, N&B sub_8264E570)
BANJO_RENUT_HOOK(sub_8216B040, D3DDevice_SetPixelShaderConstantI_hook);  // D3DDevice_SetPixelShaderConstantI  // feedingfrenzy2-nx: sig 1.00 (Peggle 1 sub_8242C5F8, N&B sub_8264E5C8)
// feedingfrenzy2-nx: not linked or not called in XDK 6534: BANJO_RENUT_HOOK(sub_XXXXXXXX, D3DDevice_EndIndexedVertices_hook);  // D3DDevice_EndIndexedVertices  // Peggle 1 sub_8243B808, N&B sub_826561F8

// Render state getters, by their index in N&B's table (off_82E506F0, field 0).
BANJO_RENUT_HOOK(sub_82171D78, GetRenderStateHook<10>);  // D3DDevice_GetRenderState_ZEnable  // feedingfrenzy2-nx: render state 10 getter (the state tables) (Peggle 1 sub_82434678, N&B sub_8264C178)
BANJO_RENUT_HOOK(sub_82171DD8, GetRenderStateHook<11>);  // D3DDevice_GetRenderState_ZFunc  // feedingfrenzy2-nx: render state 11 getter (the state tables) (Peggle 1 sub_824346D8, N&B sub_8264C1B0)
BANJO_RENUT_HOOK(sub_82171DA0, GetRenderStateHook<12>);  // D3DDevice_GetRenderState_ZWriteEnable  // feedingfrenzy2-nx: render state 12 getter (the state tables) (Peggle 1 sub_824346A0, N&B sub_8264C1A0)
BANJO_RENUT_HOOK(sub_82171640, GetRenderStateHook<13>);  // D3DDevice_GetRenderState_FillMode  // feedingfrenzy2-nx: render state 13 getter (the state tables) (Peggle 1 sub_82433EF0, N&B sub_8264BE88)
BANJO_RENUT_HOOK(sub_82171610, GetRenderStateHook<14>);  // D3DDevice_GetRenderState_CullMode  // feedingfrenzy2-nx: render state 14 getter (the state tables) (Peggle 1 sub_82433EC0, N&B sub_8264BE78)
BANJO_RENUT_HOOK(sub_82171708, GetRenderStateHook<15>);  // D3DDevice_GetRenderState_AlphaBlendEnable  // feedingfrenzy2-nx: render state 15 getter (the state tables) (Peggle 1 sub_82434030, N&B sub_8264BED0)
BANJO_RENUT_HOOK(sub_82171AA0, GetRenderStateHook<16>);  // D3DDevice_GetRenderState_SeparateAlphaBlendEnable  // feedingfrenzy2-nx: render state 16 getter (the state tables) (Peggle 1 sub_824343C8, N&B sub_8264BF40)
BANJO_RENUT_HOOK(sub_82171BE8, GetRenderStateHook<17>);  // D3DDevice_GetRenderState_BlendFactor  // feedingfrenzy2-nx: render state 17 getter (the state tables) (Peggle 1 sub_824344F8, N&B sub_8264BFE8)
BANJO_RENUT_HOOK(sub_82171828, GetRenderStateHook<18>);  // D3DDevice_GetRenderState_SrcBlend  // feedingfrenzy2-nx: render state 18 getter (the state tables) (Peggle 1 sub_82434150, N&B sub_8264BEF0)
BANJO_RENUT_HOOK(sub_821718B8, GetRenderStateHook<19>);  // D3DDevice_GetRenderState_DestBlend  // feedingfrenzy2-nx: render state 19 getter (the state tables) (Peggle 1 sub_824341E0, N&B sub_8264BF00)
BANJO_RENUT_HOOK(sub_82171798, GetRenderStateHook<20>);  // D3DDevice_GetRenderState_BlendOp  // feedingfrenzy2-nx: render state 20 getter (the state tables) (Peggle 1 sub_824340C0, N&B sub_8264BEE0)
BANJO_RENUT_HOOK(sub_82171998, GetRenderStateHook<21>);  // D3DDevice_GetRenderState_SrcBlendAlpha  // feedingfrenzy2-nx: render state 21 getter (the state tables) (Peggle 1 sub_824342C0, N&B sub_8264BF20)
BANJO_RENUT_HOOK(sub_82171A08, GetRenderStateHook<22>);  // D3DDevice_GetRenderState_DestBlendAlpha  // feedingfrenzy2-nx: render state 22 getter (the state tables) (Peggle 1 sub_82434330, N&B sub_8264BF30)
BANJO_RENUT_HOOK(sub_82171928, GetRenderStateHook<23>);  // D3DDevice_GetRenderState_BlendOpAlpha  // feedingfrenzy2-nx: render state 23 getter (the state tables) (Peggle 1 sub_82434250, N&B sub_8264BF10)
BANJO_RENUT_HOOK(sub_82171678, GetRenderStateHook<24>);  // D3DDevice_GetRenderState_AlphaTestEnable  // feedingfrenzy2-nx: render state 24 getter (the state tables) (Peggle 1 sub_82433F28, N&B sub_8264BEC0)
BANJO_RENUT_HOOK(sub_82171AE8, GetRenderStateHook<25>);  // D3DDevice_GetRenderState_AlphaRef  // feedingfrenzy2-nx: render state 25 getter (the state tables) (Peggle 1 sub_82434410, N&B sub_8264BF88)
BANJO_RENUT_HOOK(sub_82171B38, GetRenderStateHook<26>);  // D3DDevice_GetRenderState_AlphaFunc  // feedingfrenzy2-nx: render state 26 getter (the state tables) (Peggle 1 sub_82434458, N&B sub_8264BFD8)
BANJO_RENUT_HOOK(sub_82171E20, GetRenderStateHook<27>);  // D3DDevice_GetRenderState_StencilEnable  // feedingfrenzy2-nx: render state 27 getter (the state tables) (Peggle 1 sub_82434720, N&B sub_8264C1F8)
BANJO_RENUT_HOOK(sub_82171E50, GetRenderStateHook<28>);  // D3DDevice_GetRenderState_TwoSidedStencilMode  // feedingfrenzy2-nx: render state 28 getter (the state tables) (Peggle 1 sub_82434750, N&B sub_8264C228)
BANJO_RENUT_HOOK(sub_82171EB8, GetRenderStateHook<29>);  // D3DDevice_GetRenderState_StencilFail  // feedingfrenzy2-nx: render state 29 getter (the state tables) (Peggle 1 sub_824347B8, N&B sub_8264C290)
BANJO_RENUT_HOOK(sub_82171EF0, GetRenderStateHook<30>);  // D3DDevice_GetRenderState_StencilZFail  // feedingfrenzy2-nx: render state 30 getter (the state tables) (Peggle 1 sub_824347F0, N&B sub_8264C2C8)
BANJO_RENUT_HOOK(sub_82171F20, GetRenderStateHook<31>);  // D3DDevice_GetRenderState_StencilPass  // feedingfrenzy2-nx: render state 31 getter (the state tables) (Peggle 1 sub_82434820, N&B sub_8264C2F8)
BANJO_RENUT_HOOK(sub_82171E80, GetRenderStateHook<32>);  // D3DDevice_GetRenderState_StencilFunc  // feedingfrenzy2-nx: render state 32 getter (the state tables) (Peggle 1 sub_82434780, N&B sub_8264C258)
BANJO_RENUT_HOOK(sub_82172018, GetRenderStateHook<33>);  // D3DDevice_GetRenderState_StencilRef  // feedingfrenzy2-nx: render state 33 getter (the state tables) (Peggle 1 sub_82434918, N&B sub_8264C3F0)
BANJO_RENUT_HOOK(sub_82172038, GetRenderStateHook<34>);  // D3DDevice_GetRenderState_StencilMask  // feedingfrenzy2-nx: render state 34 getter (the state tables) (Peggle 1 sub_82434938, N&B sub_8264C410)
BANJO_RENUT_HOOK(sub_82172058, GetRenderStateHook<35>);  // D3DDevice_GetRenderState_StencilWriteMask  // feedingfrenzy2-nx: render state 35 getter (the state tables) (Peggle 1 sub_82434958, N&B sub_8264C430)
BANJO_RENUT_HOOK(sub_82171F88, GetRenderStateHook<36>);  // D3DDevice_GetRenderState_CCWStencilFail  // feedingfrenzy2-nx: render state 36 getter (the state tables) (Peggle 1 sub_82434888, N&B sub_8264C360)
BANJO_RENUT_HOOK(sub_82171FC0, GetRenderStateHook<37>);  // D3DDevice_GetRenderState_CCWStencilZFail  // feedingfrenzy2-nx: render state 37 getter (the state tables) (Peggle 1 sub_824348C0, N&B sub_8264C398)
BANJO_RENUT_HOOK(sub_82171FF0, GetRenderStateHook<38>);  // D3DDevice_GetRenderState_CCWStencilPass  // feedingfrenzy2-nx: render state 38 getter (the state tables) (Peggle 1 sub_824348F0, N&B sub_8264C3C8)
BANJO_RENUT_HOOK(sub_82171F50, GetRenderStateHook<39>);  // D3DDevice_GetRenderState_CCWStencilFunc  // feedingfrenzy2-nx: render state 39 getter (the state tables) (Peggle 1 sub_82434850, N&B sub_8264C328)
BANJO_RENUT_HOOK(sub_82172078, GetRenderStateHook<40>);  // D3DDevice_GetRenderState_CCWStencilRef  // feedingfrenzy2-nx: render state 40 getter (the state tables) (Peggle 1 sub_82434978, N&B sub_8264C450)
BANJO_RENUT_HOOK(sub_82172098, GetRenderStateHook<41>);  // D3DDevice_GetRenderState_CCWStencilMask  // feedingfrenzy2-nx: render state 41 getter (the state tables) (Peggle 1 sub_82434998, N&B sub_8264C470)
BANJO_RENUT_HOOK(sub_821720B8, GetRenderStateHook<42>);  // D3DDevice_GetRenderState_CCWStencilWriteMask  // feedingfrenzy2-nx: render state 42 getter (the state tables) (Peggle 1 sub_824349B8, N&B sub_8264C490)
BANJO_RENUT_HOOK(sub_82172100, GetRenderStateHook<43>);  // D3DDevice_GetRenderState_ClipPlaneEnable  // feedingfrenzy2-nx: render state 43 getter (the state tables) (Peggle 1 sub_82434A00, N&B sub_8264C498)
BANJO_RENUT_HOOK(sub_82172438, GetRenderStateHook<44>);  // D3DDevice_GetRenderState_PointSize  // feedingfrenzy2-nx: render state 44 getter (the state tables) (Peggle 1 sub_82434D30, N&B sub_8264C710)
BANJO_RENUT_HOOK(sub_82172490, GetRenderStateHook<45>);  // D3DDevice_GetRenderState_PointSizeMin  // feedingfrenzy2-nx: render state 45 getter (the state tables) (Peggle 1 sub_82434D80, N&B sub_8264C760)
BANJO_RENUT_HOOK(sub_821723E0, GetRenderStateHook<46>);  // D3DDevice_GetRenderState_PointSpriteEnable  // feedingfrenzy2-nx: render state 46 getter (the state tables) (Peggle 1 sub_82434CE0, N&B sub_8264C6B8)
BANJO_RENUT_HOOK(sub_821724E8, GetRenderStateHook<47>);  // D3DDevice_GetRenderState_PointSizeMax  // feedingfrenzy2-nx: render state 47 getter (the state tables) (Peggle 1 sub_82434DD0, N&B sub_8264C770)
BANJO_RENUT_HOOK(sub_821722A8, GetRenderStateHook<48>);  // D3DDevice_GetRenderState_MultiSampleAntiAlias  // feedingfrenzy2-nx: render state 48 getter (the state tables) (Peggle 1 sub_82434BA8, N&B sub_8264C5B8)
BANJO_RENUT_HOOK(sub_821722D0, GetRenderStateHook<49>);  // D3DDevice_GetRenderState_MultiSampleMask  // feedingfrenzy2-nx: render state 49 getter (the state tables) (Peggle 1 sub_82434BD0, N&B sub_8264C5E0)
BANJO_RENUT_HOOK(sub_82172110, GetRenderStateHook<50>);  // D3DDevice_GetRenderState_ScissorTestEnable  // feedingfrenzy2-nx: render state 50 getter (the state tables) (Peggle 1 sub_82434A10, N&B sub_8264C4B8)
BANJO_RENUT_HOOK(sub_821721C0, GetRenderStateHook<51>);  // D3DDevice_GetRenderState_SlopeScaleDepthBias  // feedingfrenzy2-nx: render state 51 getter (the state tables) (Peggle 1 sub_82434AC0, N&B sub_8264C568)
BANJO_RENUT_HOOK(sub_82172278, GetRenderStateHook<52>);  // D3DDevice_GetRenderState_DepthBias  // feedingfrenzy2-nx: render state 52 getter (the state tables) (Peggle 1 sub_82434B78, N&B sub_8264C588)
BANJO_RENUT_HOOK(sub_82172310, GetRenderStateHook<53>);  // D3DDevice_GetRenderState_ColorWriteEnable  // feedingfrenzy2-nx: render state 53 getter (the state tables) (Peggle 1 sub_82434C10, N&B sub_8264C5E8)
BANJO_RENUT_HOOK(sub_82172350, GetRenderStateHook<54>);  // D3DDevice_GetRenderState_ColorWriteEnable1  // feedingfrenzy2-nx: render state 54 getter (the state tables) (Peggle 1 sub_82434C50, N&B sub_8264C628)
BANJO_RENUT_HOOK(sub_82172390, GetRenderStateHook<55>);  // D3DDevice_GetRenderState_ColorWriteEnable2  // feedingfrenzy2-nx: render state 55 getter (the state tables) (Peggle 1 sub_82434C90, N&B sub_8264C668)
BANJO_RENUT_HOOK(sub_821723D0, GetRenderStateHook<56>);  // D3DDevice_GetRenderState_ColorWriteEnable3  // feedingfrenzy2-nx: render state 56 getter (the state tables) (Peggle 1 sub_82434CD0, N&B sub_8264C6A8)
BANJO_RENUT_HOOK(sub_82172C00, GetRenderStateHook<57>);  // D3DDevice_GetRenderState_TessellationMode  // feedingfrenzy2-nx: render state 57 getter (the state tables) (Peggle 1 sub_824355E8, N&B sub_8264CDE8)
BANJO_RENUT_HOOK(sub_82172B90, GetRenderStateHook<58>);  // D3DDevice_GetRenderState_MinTessellationLevel  // feedingfrenzy2-nx: render state 58 getter (the state tables) (Peggle 1 sub_82435578, N&B sub_8264CD78)
BANJO_RENUT_HOOK(sub_82172BC8, GetRenderStateHook<59>);  // D3DDevice_GetRenderState_MaxTessellationLevel  // feedingfrenzy2-nx: render state 59 getter (the state tables) (Peggle 1 sub_824355B0, N&B sub_8264CDB0)
BANJO_RENUT_HOOK(sub_82172758, GetRenderStateHook<60>);  // D3DDevice_GetRenderState_Wrap0  // feedingfrenzy2-nx: render state 60 getter (the state tables) (Peggle 1 sub_824350C0, N&B sub_8264C9E0)
BANJO_RENUT_HOOK(sub_82172768, GetRenderStateHook<61>);  // D3DDevice_GetRenderState_Wrap1  // feedingfrenzy2-nx: render state 61 getter (the state tables) (Peggle 1 sub_824350D0, N&B sub_8264C9F0)
BANJO_RENUT_HOOK(sub_82172778, GetRenderStateHook<62>);  // D3DDevice_GetRenderState_Wrap2  // feedingfrenzy2-nx: render state 62 getter (the state tables) (Peggle 1 sub_824350E0, N&B sub_8264CA00)
BANJO_RENUT_HOOK(sub_82172788, GetRenderStateHook<63>);  // D3DDevice_GetRenderState_Wrap3  // feedingfrenzy2-nx: render state 63 getter (the state tables) (Peggle 1 sub_824350F0, N&B sub_8264CA10)
BANJO_RENUT_HOOK(sub_82172798, GetRenderStateHook<64>);  // D3DDevice_GetRenderState_Wrap4  // feedingfrenzy2-nx: render state 64 getter (the state tables) (Peggle 1 sub_82435100, N&B sub_8264CA20)
BANJO_RENUT_HOOK(sub_821727A8, GetRenderStateHook<65>);  // D3DDevice_GetRenderState_Wrap5  // feedingfrenzy2-nx: render state 65 getter (the state tables) (Peggle 1 sub_82435110, N&B sub_8264CA30)
BANJO_RENUT_HOOK(sub_821727B8, GetRenderStateHook<66>);  // D3DDevice_GetRenderState_Wrap6  // feedingfrenzy2-nx: render state 66 getter (the state tables) (Peggle 1 sub_82435120, N&B sub_8264CA40)
BANJO_RENUT_HOOK(sub_821727C8, GetRenderStateHook<67>);  // D3DDevice_GetRenderState_Wrap7  // feedingfrenzy2-nx: render state 67 getter (the state tables) (Peggle 1 sub_82435130, N&B sub_8264CA50)
BANJO_RENUT_HOOK(sub_821727D8, GetRenderStateHook<68>);  // D3DDevice_GetRenderState_Wrap8  // feedingfrenzy2-nx: render state 68 getter (the state tables) (Peggle 1 sub_82435140, N&B sub_8264CA60)
BANJO_RENUT_HOOK(sub_821727E8, GetRenderStateHook<69>);  // D3DDevice_GetRenderState_Wrap9  // feedingfrenzy2-nx: render state 69 getter (the state tables) (Peggle 1 sub_82435150, N&B sub_8264CA70)
BANJO_RENUT_HOOK(sub_821727F8, GetRenderStateHook<70>);  // D3DDevice_GetRenderState_Wrap10  // feedingfrenzy2-nx: render state 70 getter (the state tables) (Peggle 1 sub_82435160, N&B sub_8264CA80)
BANJO_RENUT_HOOK(sub_82172808, GetRenderStateHook<71>);  // D3DDevice_GetRenderState_Wrap11  // feedingfrenzy2-nx: render state 71 getter (the state tables) (Peggle 1 sub_82435170, N&B sub_8264CA90)
BANJO_RENUT_HOOK(sub_82172818, GetRenderStateHook<72>);  // D3DDevice_GetRenderState_Wrap12  // feedingfrenzy2-nx: render state 72 getter (the state tables) (Peggle 1 sub_82435180, N&B sub_8264CAA0)
BANJO_RENUT_HOOK(sub_82172828, GetRenderStateHook<73>);  // D3DDevice_GetRenderState_Wrap13  // feedingfrenzy2-nx: render state 73 getter (the state tables) (Peggle 1 sub_82435190, N&B sub_8264CAB0)
BANJO_RENUT_HOOK(sub_82172838, GetRenderStateHook<74>);  // D3DDevice_GetRenderState_Wrap14  // feedingfrenzy2-nx: render state 74 getter (the state tables) (Peggle 1 sub_824351A0, N&B sub_8264CAC0)
BANJO_RENUT_HOOK(sub_82172848, GetRenderStateHook<75>);  // D3DDevice_GetRenderState_Wrap15  // feedingfrenzy2-nx: render state 75 getter (the state tables) (Peggle 1 sub_824351B0, N&B sub_8264CAD0)
BANJO_RENUT_HOOK(sub_82172898, GetRenderStateHook<76>);  // D3DDevice_GetRenderState_ViewportEnable  // feedingfrenzy2-nx: render state 76 getter (the state tables) (Peggle 1 sub_82435200, N&B sub_8264CAE0)
BANJO_RENUT_HOOK(sub_82172950, GetRenderStateHook<77>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable  // feedingfrenzy2-nx: render state 77 getter (the state tables) (Peggle 1 sub_82435380, N&B sub_8264CB80)
BANJO_RENUT_HOOK(sub_82172A00, GetRenderStateHook<78>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable1  // feedingfrenzy2-nx: render state 78 getter (the state tables) (Peggle 1 sub_82435418, N&B sub_8264CC18)
BANJO_RENUT_HOOK(sub_82172AB0, GetRenderStateHook<79>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable2  // feedingfrenzy2-nx: render state 79 getter (the state tables) (Peggle 1 sub_824354B0, N&B sub_8264CCB0)
BANJO_RENUT_HOOK(sub_82172B60, GetRenderStateHook<80>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable3  // feedingfrenzy2-nx: render state 80 getter (the state tables) (Peggle 1 sub_82435548, N&B sub_8264CD48)
BANJO_RENUT_HOOK(sub_82172C38, GetRenderStateHook<81>);  // D3DDevice_GetRenderState_HalfPixelOffset  // feedingfrenzy2-nx: render state 81 getter (the state tables) (Peggle 1 sub_82435620, N&B sub_8264CE20)
BANJO_RENUT_HOOK(sub_82172C68, GetRenderStateHook<82>);  // D3DDevice_GetRenderState_PrimitiveResetEnable  // feedingfrenzy2-nx: render state 82 getter (the state tables) (Peggle 1 sub_82435650, N&B sub_8264CE50)
BANJO_RENUT_HOOK(sub_82172C98, GetRenderStateHook<83>);  // D3DDevice_GetRenderState_PrimitiveResetIndex  // feedingfrenzy2-nx: render state 83 getter (the state tables) (Peggle 1 sub_82435680, N&B sub_8264CE80)
BANJO_RENUT_HOOK(sub_82172CC0, GetRenderStateHook<84>);  // D3DDevice_GetRenderState_AlphaToMaskEnable  // feedingfrenzy2-nx: render state 84 getter (the state tables) (Peggle 1 sub_824356A8, N&B sub_8264CEA8)
BANJO_RENUT_HOOK(sub_82172CF0, GetRenderStateHook<85>);  // D3DDevice_GetRenderState_AlphaToMaskOffsets  // feedingfrenzy2-nx: render state 85 getter (the state tables) (Peggle 1 sub_824356D8, N&B sub_8264CED8)
BANJO_RENUT_HOOK(sub_82172D30, GetRenderStateHook<86>);  // D3DDevice_GetRenderState_GuardBand_X  // feedingfrenzy2-nx: render state 86 getter (the state tables) (Peggle 1 sub_82435718, N&B sub_8264CF18)
BANJO_RENUT_HOOK(sub_82172D60, GetRenderStateHook<87>);  // D3DDevice_GetRenderState_GuardBand_Y  // feedingfrenzy2-nx: render state 87 getter (the state tables) (Peggle 1 sub_82435748, N&B sub_8264CF48)
BANJO_RENUT_HOOK(sub_82172D88, GetRenderStateHook<88>);  // D3DDevice_GetRenderState_DiscardBand_X  // feedingfrenzy2-nx: render state 88 getter (the state tables) (Peggle 1 sub_82435770, N&B sub_8264CF70)
BANJO_RENUT_HOOK(sub_82172DB8, GetRenderStateHook<89>);  // D3DDevice_GetRenderState_DiscardBand_Y  // feedingfrenzy2-nx: render state 89 getter (the state tables) (Peggle 1 sub_824357A0, N&B sub_8264CFA0)
BANJO_RENUT_HOOK(sub_82172E78, GetRenderStateHook<90>);  // D3DDevice_GetRenderState_HiStencilEnable  // feedingfrenzy2-nx: render state 90 getter (the state tables) (Peggle 1 sub_82435860, N&B sub_8264D008)
BANJO_RENUT_HOOK(sub_82172EA8, GetRenderStateHook<91>);  // D3DDevice_GetRenderState_HiStencilWriteEnable  // feedingfrenzy2-nx: render state 91 getter (the state tables) (Peggle 1 sub_82435890, N&B sub_8264D038)
BANJO_RENUT_HOOK(sub_82172ED8, GetRenderStateHook<92>);  // D3DDevice_GetRenderState_HiStencilFunc  // feedingfrenzy2-nx: render state 92 getter (the state tables) (Peggle 1 sub_824358C0, N&B sub_8264D068)
BANJO_RENUT_HOOK(sub_82172F00, GetRenderStateHook<93>);  // D3DDevice_GetRenderState_HiStencilRef  // feedingfrenzy2-nx: render state 93 getter (the state tables) (Peggle 1 sub_824358E8, N&B sub_8264D090)
BANJO_RENUT_HOOK(sub_82172F10, GetRenderStateHook<94>);  // D3DDevice_GetRenderState_PresentInterval  // feedingfrenzy2-nx: render state 94 getter (the state tables) (Peggle 1 sub_824358F8, N&B sub_8264D0A0)
BANJO_RENUT_HOOK(sub_82172F28, GetRenderStateHook<95>);  // D3DDevice_GetRenderState_PresentImmediateThreshold  // feedingfrenzy2-nx: render state 95 getter (the state tables) (Peggle 1 sub_82435910, N&B sub_8264D0B8)
BANJO_RENUT_HOOK(sub_82172DE0, GetRenderStateHook<96>);  // D3DDevice_GetRenderState_HiZEnable  // feedingfrenzy2-nx: render state 96 getter (the state tables) (Peggle 1 sub_824357C8, N&B sub_8264CFC8)
BANJO_RENUT_HOOK(sub_82172E48, GetRenderStateHook<97>);  // D3DDevice_GetRenderState_HiZWriteEnable  // feedingfrenzy2-nx: render state 97 getter (the state tables) (Peggle 1 sub_82435830, N&B sub_8264CFD8)
BANJO_RENUT_HOOK(sub_82171C90, GetRenderStateHook<98>);  // D3DDevice_GetRenderState_LastPixel  // feedingfrenzy2-nx: render state 98 getter (the state tables) (Peggle 1 sub_82434590, N&B sub_8264C088)
BANJO_RENUT_HOOK(sub_82171CE0, GetRenderStateHook<99>);  // D3DDevice_GetRenderState_LineWidth  // feedingfrenzy2-nx: render state 99 getter (the state tables) (Peggle 1 sub_824345E0, N&B sub_8264C0D8)
BANJO_RENUT_HOOK(sub_82171D30, GetRenderStateHook<100>);  // D3DDevice_GetRenderState_Buffer2Frames  // feedingfrenzy2-nx: render state 100 getter (the state tables) (Peggle 1 sub_82434630, N&B sub_8264C130)

// Sampler state getters (off_82E50BD0, field 0).
BANJO_RENUT_HOOK(sub_821736B0, GetSamplerStateHook<0>);  // D3DDevice_GetSamplerState_AddressU  // feedingfrenzy2-nx: sampler state 0 getter (the state tables) (Peggle 1 sub_824360D0, N&B sub_8264D600)
BANJO_RENUT_HOOK(sub_82173700, GetSamplerStateHook<1>);  // D3DDevice_GetSamplerState_AddressV  // feedingfrenzy2-nx: sampler state 1 getter (the state tables) (Peggle 1 sub_82436120, N&B sub_8264D618)
BANJO_RENUT_HOOK(sub_82173750, GetSamplerStateHook<2>);  // D3DDevice_GetSamplerState_AddressW  // feedingfrenzy2-nx: sampler state 2 getter (the state tables) (Peggle 1 sub_82436170, N&B sub_8264D668)
BANJO_RENUT_HOOK(sub_82173658, GetSamplerStateHook<3>);  // D3DDevice_GetSamplerState_BorderColor  // feedingfrenzy2-nx: sampler state 3 getter (the state tables) (Peggle 1 sub_82436078, N&B sub_8264D5E0)
BANJO_RENUT_HOOK(sub_82173188, GetSamplerStateHook<4>);  // D3DDevice_GetSamplerState_MagFilter  // feedingfrenzy2-nx: sampler state 4 getter (the state tables) (Peggle 1 sub_82435B90, N&B sub_8264D1A8)
BANJO_RENUT_HOOK(sub_82172FF8, GetSamplerStateHook<5>);  // D3DDevice_GetSamplerState_MinFilter  // feedingfrenzy2-nx: sampler state 5 getter (the state tables) (Peggle 1 sub_824359E8, N&B sub_8264D0C8)
BANJO_RENUT_HOOK(sub_82173298, GetSamplerStateHook<6>);  // D3DDevice_GetSamplerState_MipFilter  // feedingfrenzy2-nx: sampler state 6 getter (the state tables) (Peggle 1 sub_82435CA8, N&B sub_8264D280)
BANJO_RENUT_HOOK(sub_821734C8, GetSamplerStateHook<7>);  // D3DDevice_GetSamplerState_MipMapLodBias  // feedingfrenzy2-nx: sampler state 7 getter (the state tables) (Peggle 1 sub_82435EE8, N&B sub_8264D4C0)
BANJO_RENUT_HOOK(sub_82173578, GetSamplerStateHook<8>);  // D3DDevice_GetSamplerState_MaxMipLevel  // feedingfrenzy2-nx: sampler state 8 getter (the state tables) (Peggle 1 sub_82435F98, N&B sub_8264D500)
BANJO_RENUT_HOOK(sub_821733B0, GetSamplerStateHook<9>);  // D3DDevice_GetSamplerState_MaxAnisotropy  // feedingfrenzy2-nx: sampler state 9 getter (the state tables) (Peggle 1 sub_82435DD0, N&B sub_8264D3A8)
BANJO_RENUT_HOOK(sub_82173248, GetSamplerStateHook<10>);  // D3DDevice_GetSamplerState_MagFilterZ  // feedingfrenzy2-nx: sampler state 10 getter (the state tables) (Peggle 1 sub_82435C58, N&B sub_8264D270)
BANJO_RENUT_HOOK(sub_821730B8, GetSamplerStateHook<11>);  // D3DDevice_GetSamplerState_MinFilterZ  // feedingfrenzy2-nx: sampler state 11 getter (the state tables) (Peggle 1 sub_82435AB8, N&B sub_8264D198)
BANJO_RENUT_HOOK(sub_82173338, GetSamplerStateHook<12>);  // D3DDevice_GetSamplerState_SeparateZFilterEnable  // feedingfrenzy2-nx: sampler state 12 getter (the state tables) (Peggle 1 sub_82435D58, N&B sub_8264D330)
BANJO_RENUT_HOOK(sub_821735F8, GetSamplerStateHook<13>);  // D3DDevice_GetSamplerState_MinMipLevel  // feedingfrenzy2-nx: sampler state 13 getter (the state tables) (Peggle 1 sub_82436018, N&B sub_8264D580)
BANJO_RENUT_HOOK(sub_821737A8, GetSamplerStateHook<14>);  // D3DDevice_GetSamplerState_TrilinearThreshold  // feedingfrenzy2-nx: sampler state 14 getter (the state tables) (Peggle 1 sub_824361C8, N&B sub_8264D6C0)
BANJO_RENUT_HOOK(sub_82173420, GetSamplerStateHook<15>);  // D3DDevice_GetSamplerState_AnisotropyBias  // feedingfrenzy2-nx: sampler state 15 getter (the state tables) (Peggle 1 sub_82435E40, N&B sub_8264D418)
BANJO_RENUT_HOOK(sub_82173800, GetSamplerStateHook<16>);  // D3DDevice_GetSamplerState_HGradientExpBias  // feedingfrenzy2-nx: sampler state 16 getter (the state tables) (Peggle 1 sub_82436220, N&B sub_8264D718)
BANJO_RENUT_HOOK(sub_82173858, GetSamplerStateHook<17>);  // D3DDevice_GetSamplerState_VGradientExpBias  // feedingfrenzy2-nx: sampler state 17 getter (the state tables) (Peggle 1 sub_82436278, N&B sub_8264D770)
BANJO_RENUT_HOOK(sub_821738B0, GetSamplerStateHook<18>);  // D3DDevice_GetSamplerState_WhiteBorderColorW  // feedingfrenzy2-nx: sampler state 18 getter (the state tables) (Peggle 1 sub_824362D0, N&B sub_8264D7C8)
BANJO_RENUT_HOOK(sub_82173910, GetSamplerStateHook<19>);  // D3DDevice_GetSamplerState_PointBorderEnable  // feedingfrenzy2-nx: sampler state 19 getter (the state tables) (Peggle 1 sub_82436330, N&B sub_8264D828)
