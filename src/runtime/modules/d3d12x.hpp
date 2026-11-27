#pragma once
#include "core/target.hpp"
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <dxgi.h>
#include <string_view>
#include <vector>

// Direct3D 12 as both consoles have it (d3d12_x.dll on Xbox One, d3d12_xs.dll on Xbox Series
// X|S) on the host's Direct3D 12.
namespace xv::runtime {
class Registry;
namespace modules::d3d12x {
// `module` is the name the console gives the DLL
void install(Registry &registry, std::string_view module, Target target);
// The console's device is also its DXGI device, whose adapter leads to a factory. A console
// whose titles make swap chains through that factory wraps the host's adapter here.
using AdapterWrapper = HRESULT (*)(IDXGIAdapter *host, IDXGIAdapter **adapter);
void set_adapter_wrapper(AdapterWrapper wrapper);
// the title's first direct queue, null before it has one
ID3D12CommandQueue *direct_queue();
// a Direct3D 12 resource handed to the title: the console's methods where they differ
void console_d3d12_resource(void *resource);
// The console's command list on a host one, as the device makes it for the title, with a
// reference of its own to the host's. Without `enhanced_barriers` its Barrier is as on a host
// that has none. Exposed for the tests.
void *console_command_list(ID3D12GraphicsCommandList *host, bool enhanced_barriers = true);
// Whether the host made its pipeline of one the title made, which is made the first time a
// list is given it. Exposed for the tests.
HRESULT made_pipeline(void *pipeline);
// a command allocator or fence handed to the title: the console's methods past the desktop's
void console_command_allocator(void *allocator);
void console_fence(void *fence);
// Code at [code, code + size) calls the console's Direct3D 12 objects as the title's own code
// does, until it is given to stop_calling_as_title. Exposed for the tests, which have no title.
void call_as_title(const void *code, std::size_t size);
void stop_calling_as_title(const void *code);
// The title's layouts of queues and lists unknown again, as before its calls showed them.
// Exposed for the tests, each of which stands for a title of its own.
void forget_layouts();
// why the host's device stopped, and what its debug layer said, to the log
void report_failure();
// The pipelines a title's lists were given in its earlier runs, kept in the shader cache's
// "pipelines" folder, made for the host once each without the title, so that the host
// driver's blobs of the slow ones are there for the next run. How many were made.
std::size_t warm_pipelines(unsigned threads);
// a texture's first subresource, copied to the CPU through the title's direct queue
// (XV_D3D_PROBE)
bool read_back(ID3D12Resource *resource, std::vector<std::uint8_t> &data, unsigned &pitch,
               unsigned &width, unsigned &height, DXGI_FORMAT &format,
               D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT);
// the render target the title bound last, for XV_D3D_PROBE
ID3D12Resource *last_render_target();
// the first render target of each size and format the title bound
std::vector<ID3D12Resource *> bound_render_targets();
// how many draws went into a render target so far
std::uint64_t draws_into_target(ID3D12Resource *target);
// XV_D3D_PROBE: the average colour of a picture to the log, and the picture itself as a
// 32-bit BMP named after `what` in the temporary folder
void report_picture(const std::uint8_t *data, unsigned pitch, unsigned width, unsigned height,
                    DXGI_FORMAT format, const char *what);
} // namespace modules::d3d12x
} // namespace xv::runtime
