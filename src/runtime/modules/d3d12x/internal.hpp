#pragma once
#include <cstdint>
#include <d3d12.h>
#include <vector>

// What the console's device and queue have for showing frames, which the host does with a
// swap chain: present.cpp.
namespace xv::runtime::modules::d3d12x {
// the state the title's own barriers left a render target in, `unknown` if it made none
D3D12_RESOURCE_STATES tracked_state(ID3D12Resource *resource, D3D12_RESOURCE_STATES unknown);
// how many frames the title has presented
std::uint64_t frames_shown();
// XV_CAPTURE (capture.cpp): whether this frame is one to keep; a target or depth buffer the
// title binds, a draw into the target bound first, and the frame presented
bool capturing();
void capture_bound(ID3D12Resource *resource, bool depth);
void capture_draw(ID3D12Resource *target);
void capture_frame(ID3D12Resource *picture);
// a mesh draw, direct or indirect, for XV_CAPTURE_AUTO
void count_mesh_draw();
// the textures the title writes through memory of ours (scratch_memory), a reference each
std::vector<ID3D12Resource *> written_textures();
// ID3D12CommandQueue::PresentX on the host's queue
HRESULT present_x(ID3D12CommandQueue *queue, UINT planes, const void *plane_parameters,
                  const void *present_parameters);
// ID3D12Device::SetFrameIntervalX, ScheduleFrameEventX, WaitFrameEventX, GetFrameStatisticsX
HRESULT STDMETHODCALLTYPE set_frame_interval(ID3D12Device *device, void *output, UINT microseconds,
                                             UINT period, UINT flags);
HRESULT STDMETHODCALLTYPE schedule_frame_event(ID3D12Device *device, UINT type, UINT offset,
                                               void *signals, UINT flags);
HRESULT STDMETHODCALLTYPE wait_frame_event(ID3D12Device *device, UINT type, UINT timeout,
                                           void *waits, UINT flags, UINT64 *token);
HRESULT STDMETHODCALLTYPE frame_statistics(ID3D12Device *device, UINT64 token, UINT types,
                                           UINT *count, void *statistics);
} // namespace xv::runtime::modules::d3d12x
