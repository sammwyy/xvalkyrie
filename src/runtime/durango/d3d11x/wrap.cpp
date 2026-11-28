#include "runtime/durango/d3d11x/wrap.hpp"
#include "backends/graphics/backend.hpp"
#include "runtime/common/com_forward.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/d3d11x/descriptors.hpp"
#include "runtime/durango/d3d11x/layout.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <intrin.h>
#include <map>
#include <mutex>
#include <new>
#include <utility>

namespace xv::runtime::durango::d3d11x {
HRESULT device_dxgi(IUnknown *host_device, REFIID iid, void **object);
namespace {
using Word = std::uint64_t;
constexpr std::size_t argument_count = 11; // after the object itself
constexpr std::uint64_t magic = 0x5853'4433'4431'3158;

// Sits right before the object the title sees, whose first word is the vtable.
struct Header {
    std::uint64_t magic;
    IUnknown *host;
    const Layout *layout;
    Kind kind;
    std::atomic<ULONG> references;
};
Header *header(void *object) { return static_cast<Header *>(object) - 1; }
bool ours(void *object) {
    if (!object || reinterpret_cast<std::uintptr_t>(object) < 0x10000 + sizeof(Header))
        return false;
    __try {
        return header(object)->magic == magic;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::mutex mutex;
std::map<IUnknown *, void *> objects; // host -> ours, for as long as ours lives

bool trace() {
    static const bool enabled = std::getenv("XV_D3D_TRACE") != nullptr;
    return enabled;
}
// true the first time a method of a layout is reached
bool first_call(const Layout &layout, bool in_object, std::size_t index) {
    static std::atomic<bool> seen[16][2][object_slots];
    const auto which = static_cast<std::size_t>(&layout - &d3d11x::layout(Kind::device));
    return !seen[which][in_object][index].exchange(true);
}

Word call(void *self, const Method &method, bool in_object, std::size_t index, Word *arguments,
          void *caller) {
    auto *const info = header(self);
    const auto where = in_object ? "offset" : "slot";
    const auto position = in_object ? index * 8 : index;
    if (method.action == Method::ignore) {
        if (trace() && first_call(*info->layout, in_object, index)) {
            std::fprintf(stderr,
                         "[Runtime] Direct3D %s %s %zu ignored, from image+0x%llx, arguments %llx "
                         "%llx %llx %llx\n",
                         info->layout->name, where, position,
                         static_cast<unsigned long long>(detail::guest_offset(caller)),
                         arguments[0], arguments[1], arguments[2], arguments[3]);
            std::fflush(stderr);
        }
        return 0;
    }
    if (method.action != Method::forward) {
        if (first_call(*info->layout, in_object, index)) {
            std::fprintf(stderr,
                         "[Runtime] unimplemented Direct3D call: %s %s %zu from image+0x%llx, "
                         "arguments %llx %llx %llx\n",
                         info->layout->name, where, position,
                         static_cast<unsigned long long>(detail::guest_offset(caller)),
                         arguments[0], arguments[1], arguments[2]);
            std::fflush(stderr);
        }
        return static_cast<Word>(E_NOTIMPL);
    }
    if (trace() && first_call(*info->layout, in_object, index)) {
        std::fprintf(stderr,
                     "[Runtime] Direct3D %s %s %zu -> host slot %u, from image+0x%llx, "
                     "arguments %llx %llx %llx %llx\n",
                     info->layout->name, where, position, method.host,
                     static_cast<unsigned long long>(detail::guest_offset(caller)), arguments[0],
                     arguments[1], arguments[2], arguments[3]);
        std::fflush(stderr);
    }
    // XV_D3D_SEQ=<frame>: every forwarded call of that frame in order, with the first object
    // of the array it passes
    if (static const char *const at = std::getenv("XV_D3D_SEQ");
        at && graphics::frames_presented() == static_cast<std::uint64_t>(std::atoi(at))) {
        MEMORY_BASIC_INFORMATION memory{};
        void *first = nullptr;
        const auto pointer = arguments[method.host == 33 ? 1 : method.host == 48 ? 3 : 2];
        if (pointer > 0x10000 &&
            VirtualQuery(reinterpret_cast<void *>(pointer), &memory, sizeof memory) &&
            memory.State == MEM_COMMIT)
            first = *reinterpret_cast<void **>(pointer);
        if (method.host == 48 && first && memory.RegionSize >= 0x10000) {
            const auto *bytes = reinterpret_cast<const std::uint8_t *>(pointer);
            unsigned low = 255, high = 0;
            for (std::size_t i = 0; i < 0x10000; ++i)
                low = std::min<unsigned>(low, bytes[i]), high = std::max<unsigned>(high, bytes[i]);
            std::fprintf(stderr, "[Seq] source bytes between %u and %u, pitch %llu\n", low, high,
                         arguments[4]);
        }
        std::fprintf(stderr, "[Seq] %lu %p %s %s %zu host %u: %llx %llx %llx %llx [%p%s]\n",
                     GetCurrentThreadId(), self, info->layout->name, where, position, method.host,
                     arguments[0], arguments[1], arguments[2], arguments[3], first,
                     first && unwrap(first) != first ? " ours" : "");
    }
    if (method.draws && info->layout->apply_state)
        info->layout->apply_state(self);
    // argument N of the method is arguments[N - 1]
    constexpr std::size_t array_limit = 256;
    void *arrays[3][array_limit];
    for (std::size_t i = 0; i < method.fixes.size(); ++i) {
        const auto &fix = method.fixes[i];
        auto &value = arguments[fix.argument - 1];
        if (fix.type == Fix::in) {
            value = reinterpret_cast<Word>(unwrap(reinterpret_cast<void *>(value)));
        } else if (fix.type == Fix::in_array && value) {
            const auto count = static_cast<std::uint32_t>(arguments[fix.other - 1]);
            if (count > array_limit)
                return static_cast<Word>(E_INVALIDARG);
            auto *const source = reinterpret_cast<void **>(value);
            for (std::uint32_t item = 0; item < count; ++item)
                arrays[i][item] = unwrap(source[item]);
            value = reinterpret_cast<Word>(arrays[i]);
        }
    }
    using Function =
        Word (*)(void *, Word, Word, Word, Word, Word, Word, Word, Word, Word, Word, Word);
    const auto function =
        reinterpret_cast<Function>((*reinterpret_cast<void ***>(info->host))[method.host]);
    const auto *const a = arguments;
    const auto result =
        function(info->host, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]);
    report_debug_messages(info->layout->name, where, position);
    // A creation the host refuses is usually a console-only value in the description,
    // which is the first argument: show it.
    if (FAILED(static_cast<HRESULT>(result)) && method.fixes[0].type != Fix::none &&
        (method.fixes[0].type == Fix::out_checked || method.fixes[1].type == Fix::out_checked)) {
        std::fprintf(stderr, "[Runtime] Direct3D %s %s %zu failed: 0x%08lx, from image+0x%llx",
                     info->layout->name, where, position, static_cast<unsigned long>(result),
                     static_cast<unsigned long long>(detail::guest_offset(caller)));
        MEMORY_BASIC_INFORMATION memory{};
        const auto *const words = reinterpret_cast<const std::uint32_t *>(arguments[0]);
        if (words && VirtualQuery(words, &memory, sizeof memory) && memory.State == MEM_COMMIT) {
            std::fprintf(stderr, ", description");
            for (int word = 0; word < 12; ++word)
                std::fprintf(stderr, " %x", words[word]);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
    for (const auto &fix : method.fixes) {
        if (fix.type != Fix::out && fix.type != Fix::out_checked && fix.type != Fix::out_iid &&
            fix.type != Fix::out_array)
            continue;
        auto *const slot = reinterpret_cast<void **>(arguments[fix.argument - 1]);
        if (fix.type == Fix::out_array) {
            const auto count = static_cast<std::uint32_t>(arguments[fix.other - 1]);
            for (std::uint32_t item = 0; slot && item < count; ++item)
                if (slot[item])
                    slot[item] = wrap(slot[item], fix.kind);
            continue;
        }
        if (!slot || (fix.type != Fix::out && FAILED(static_cast<HRESULT>(result))) || !*slot)
            continue;
        auto kind = fix.kind;
        if (fix.type == Fix::out_iid &&
            !kind_of(*reinterpret_cast<const GUID *>(arguments[fix.other - 1]), kind))
            continue; // an interface the host and the console share as it is
        *slot = wrap(*slot, kind);
    }
    return result;
}

void *facet(IUnknown *host, Kind kind);
// ID3D11VideoDevice is the host's as it is, but for the three methods that make a view of a
// texture, which the title names by its own object for it
HRESULT video_query(Forwarder *self, REFIID iid, void **object) {
    if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D11VideoDevice))
        return forwarder_host(self)->QueryInterface(iid, object);
    forwarder_add_ref(self);
    *object = self;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE decoder_output_view(Forwarder *self, void *resource, const void *desc,
                                              void **view) {
    return static_cast<ID3D11VideoDevice *>(forwarder_host(self))
        ->CreateVideoDecoderOutputView(
            static_cast<ID3D11Resource *>(unwrap(resource)),
            static_cast<const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC *>(desc),
            reinterpret_cast<ID3D11VideoDecoderOutputView **>(view));
}
HRESULT STDMETHODCALLTYPE processor_input_view(Forwarder *self, void *resource,
                                               ID3D11VideoProcessorEnumerator *enumerator,
                                               const void *desc, void **view) {
    return static_cast<ID3D11VideoDevice *>(forwarder_host(self))
        ->CreateVideoProcessorInputView(
            static_cast<ID3D11Resource *>(unwrap(resource)), enumerator,
            static_cast<const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *>(desc),
            reinterpret_cast<ID3D11VideoProcessorInputView **>(view));
}
HRESULT STDMETHODCALLTYPE processor_output_view(Forwarder *self, void *resource,
                                                ID3D11VideoProcessorEnumerator *enumerator,
                                                const void *desc, void **view) {
    return static_cast<ID3D11VideoDevice *>(forwarder_host(self))
        ->CreateVideoProcessorOutputView(
            static_cast<ID3D11Resource *>(unwrap(resource)), enumerator,
            static_cast<const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *>(desc),
            reinterpret_cast<ID3D11VideoProcessorOutputView **>(view));
}
void *video_device(ID3D11VideoDevice *host) {
    return make_forwarder(host, 20, &video_query,
                          {{7, reinterpret_cast<void *>(&decoder_output_view)},
                           {8, reinterpret_cast<void *>(&processor_input_view)},
                           {9, reinterpret_cast<void *>(&processor_output_view)}});
}
HRESULT STDMETHODCALLTYPE query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    *object = nullptr;
    auto *const info = header(self);
    Kind kind;
    const bool known = kind_of(iid, kind);
    const bool same =
        known && (kind == info->kind || (kind == Kind::resource && info->kind >= Kind::buffer &&
                                         info->kind <= Kind::texture3d));
    // the launch kit's console interfaces are objects of their own
    if (same && launch_kit() && (info->kind == Kind::device || info->kind == Kind::context) &&
        iid != __uuidof(ID3D11Device) && iid != __uuidof(ID3D11Device1) &&
        iid != __uuidof(ID3D11DeviceContext) && iid != __uuidof(ID3D11DeviceContext1)) {
        *object = facet(info->host, info->kind == Kind::device ? Kind::device_x : Kind::context_x);
        return S_OK;
    }
    if (same || iid == __uuidof(IUnknown) ||
        (iid == __uuidof(ID3D11DeviceChild) && info->kind != Kind::device &&
         info->kind != Kind::swap_chain)) {
        if (same &&
            (info->kind == Kind::child || info->kind == Kind::view || info->kind == Kind::state)) {
            IUnknown *check = nullptr;
            if (FAILED(info->host->QueryInterface(iid, reinterpret_cast<void **>(&check))))
                return E_NOINTERFACE;
            check->Release();
        }
        ++info->references;
        *object = self;
        return S_OK;
    }
    // ID3DUserDefinedAnnotation on the console is the context itself, with the PIX methods of
    // its function table
    constexpr GUID annotation{
        0xb2daad8b, 0x03d4, 0x4dbf, {0x95, 0xeb, 0x32, 0xab, 0x4b, 0x63, 0xd0, 0xab}};
    if (info->kind == Kind::context && iid == annotation) {
        ++info->references;
        *object = self;
        return S_OK;
    }
    if (info->kind == Kind::device &&
        (iid == __uuidof(IDXGIDevice) || iid == __uuidof(IDXGIDevice1) ||
         iid == __uuidof(IDXGIObject)))
        return device_dxgi(info->host, iid, object);
    // the debug layer is ours (XV_D3D_DEBUG), not the title's, which would call it with
    // console layouts
    if (iid == __uuidof(ID3D11Debug) || iid == __uuidof(ID3D11InfoQueue))
        return E_NOINTERFACE;
    const auto result = info->host->QueryInterface(iid, object);
    if (SUCCEEDED(result) && iid == __uuidof(ID3D11VideoDevice)) {
        *object = video_device(static_cast<ID3D11VideoDevice *>(*object));
        return result;
    }
    // a DXGI device by another name (titles ask with IIDs of their own): ours, so that its
    // adapter and factory lead to our swap chains
    if (SUCCEEDED(result) && info->kind == Kind::device) {
        IDXGIObject *dxgi = nullptr;
        if (SUCCEEDED(static_cast<IUnknown *>(*object)->QueryInterface(&dxgi)) &&
            static_cast<void *>(dxgi) == *object) {
            dxgi->Release();
            static_cast<IUnknown *>(*object)->Release();
            *object = nullptr;
            return device_dxgi(info->host, iid, object);
        }
        if (dxgi)
            dxgi->Release();
    }
    wchar_t text[40];
    StringFromGUID2(iid, text, 40);
    std::fprintf(stderr, "[Runtime] Direct3D %s asked for interface %ls: 0x%08lx%s\n",
                 info->layout->name, text, static_cast<unsigned long>(result),
                 SUCCEEDED(result) ? " (the host's object, desktop layout)" : "");
    std::fflush(stderr);
    return result;
}
ULONG STDMETHODCALLTYPE add_ref(void *self) { return ++header(self)->references; }
ULONG STDMETHODCALLTYPE release(void *self) {
    auto *const info = header(self);
    const auto left = --info->references;
    if (left)
        return left;
    {
        const std::lock_guard lock(mutex);
        // someone may have been handed the object again in the meantime
        if (info->references)
            return info->references;
        if (const auto found = objects.find(info->host);
            found != objects.end() && found->second == self)
            objects.erase(found);
    }
    forget_described(info->host);
    info->host->Release();
    info->magic = 0;
    info->~Header();
    std::free(info);
    return 0;
}

template <std::size_t Index, bool InObject>
Word entry(void *self, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7, Word a8,
           Word a9, Word a10, Word a11) {
    Word arguments[argument_count] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    const auto &layout = *header(self)->layout;
    return call(self, InObject ? layout.object[Index] : layout.vtable[Index], InObject, Index,
                arguments, _ReturnAddress());
}
template <bool InObject, std::size_t... Index>
constexpr std::array<void *, sizeof...(Index)> entries(std::index_sequence<Index...>) {
    return {reinterpret_cast<void *>(&entry<Index, InObject>)...};
}
const auto vtable_entries = entries<false>(std::make_index_sequence<vtable_slots>{});
const auto object_entries = entries<true>(std::make_index_sequence<object_slots>{});

void *slot_function(const Method &method, void *entry) {
    return method.action == Method::custom ? method.function : entry;
}
// One vtable per kind, built on first use.
void *const *vtable_of(Kind kind) {
    static std::array<void *, vtable_slots> tables[16];
    static std::once_flag once[16];
    const auto which = static_cast<std::size_t>(kind);
    std::call_once(once[which], [&] {
        const auto &methods = layout(kind).vtable;
        for (std::size_t slot = 0; slot < vtable_slots; ++slot)
            tables[which][slot] = slot_function(methods[slot], vtable_entries[slot]);
        tables[which][0] = reinterpret_cast<void *>(&query);
        tables[which][1] = reinterpret_cast<void *>(&add_ref);
        tables[which][2] = reinterpret_cast<void *>(&release);
    });
    return tables[which].data();
}
// An object for a console interface of `host` that is no part of its wrapper: it shares the
// host object, which every method of it ends up on, and nothing else
void *facet(IUnknown *host, Kind kind) {
    const auto &description = layout(kind);
    auto *const info = static_cast<Header *>(std::calloc(1, sizeof(Header) + sizeof(Word)));
    if (!info)
        throw std::bad_alloc();
    host->AddRef();
    new (info) Header{magic, host, &description, kind, 1};
    auto *const words = reinterpret_cast<void **>(info + 1);
    words[0] = const_cast<void **>(vtable_of(kind));
    return words;
}
} // namespace

namespace {
ID3D11InfoQueue *messages = nullptr;
}
void watch_debug_messages(void *host_device) {
    static_cast<IUnknown *>(host_device)->QueryInterface(&messages);
}
void report_debug_messages(const char *object, const char *where, std::size_t position) {
    if (!messages)
        return;
    const std::lock_guard lock(mutex);
    const auto count = messages->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T size = 0;
        messages->GetMessage(index, nullptr, &size);
        auto *const message = static_cast<D3D11_MESSAGE *>(std::malloc(size));
        if (message && SUCCEEDED(messages->GetMessage(index, message, &size)))
            std::fprintf(stderr, "[Runtime] Direct3D debug layer after %s %s %zu: %.*s\n", object,
                         where, position, static_cast<int>(message->DescriptionByteLength),
                         message->pDescription);
        std::free(message);
    }
    messages->ClearStoredMessages();
    if (count)
        std::fflush(stderr);
}

void *unwrap(void *object) { return ours(object) ? header(object)->host : object; }

void *wrap(void *host_object, Kind kind) {
    auto *const host = static_cast<IUnknown *>(host_object);
    if (!host)
        return nullptr;
    if (kind == Kind::resource) {
        auto dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        static_cast<ID3D11Resource *>(host)->GetType(&dimension);
        kind = dimension == D3D11_RESOURCE_DIMENSION_BUFFER      ? Kind::buffer
               : dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? Kind::texture1d
               : dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? Kind::texture3d
                                                                 : Kind::texture2d;
    }
    const std::lock_guard lock(mutex);
    if (const auto existing = objects.find(host); existing != objects.end()) {
        ++header(existing->second)->references;
        host->Release(); // ours already holds one
        return existing->second;
    }
    const auto &description = layout(kind);
    const auto size = sizeof(Header) + description.words * sizeof(Word);
    auto *const info = static_cast<Header *>(std::calloc(1, size));
    if (!info)
        throw std::bad_alloc();
    new (info) Header{magic, host, &description, kind, 1};
    auto *const words = reinterpret_cast<void **>(info + 1);
    words[0] = const_cast<void **>(vtable_of(kind));
    for (std::size_t index = 1; index < description.words; ++index) {
        const auto &method = description.object[index];
        // the three IUnknown methods come first in the in-object table as well
        const auto table = description.table;
        if (description.words > 1 && index >= table && index < table + 3)
            words[index] = const_cast<void *>(vtable_of(kind)[index - table]);
        else if (description.words > 1 && index >= table + 3)
            words[index] = slot_function(method, object_entries[index]);
    }
    if (description.words > 1 && kind != Kind::context)
        describe(words, host);
    // titles then hand resource tables to SetFastResources_Debug instead of writing packets
    if (kind == Kind::context && description.out_of_line)
        reinterpret_cast<std::uint8_t *>(words)[description.out_of_line] = 1;
    objects.emplace(host, words);
    return words;
}
} // namespace xv::runtime::durango::d3d11x
