#include "runtime/modules/dstorage.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/bcpack.hpp"
#include "runtime/modules/inflate.hpp"
#include "runtime/modules/kernel.hpp"
#include "runtime/modules/memory.hpp"
#include "runtime/modules/placement.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <execution>
#include <initializer_list>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>
#include <windows.h>

#include <d3d12.h>
#include <unknwn.h>

// DirectStorage as the Series console has it (dstorage_xs.dll, and XCRDAPI.dll for the
// factory): queues of read requests, with marks in between that set a status or signal a
// fence once everything before them is done. Here a queue does its work when it is
// submitted, on a thread of its own, in order. Reads may be inflated (zlib), then
// unpacked (the console's texture compression, BCPack). Sources and destinations may be the
// title's memory or its physical pages.
namespace xv::runtime::modules {
namespace {
constexpr GUID file_iid{
    0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr GUID factory_iid{
    0xfb3b54ac, 0xbf82, 0x463a, {0xb7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4d}};
constexpr GUID status_iid{
    0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID queue_iid{
    0xa3f6649a, 0x8c2c, 0x4b3a, {0x8b, 0x6e, 0x10, 0x18, 0x6a, 0x04, 0xb9, 0xbc}};
// IDStorageFactoryX2 and IDStorageQueueX2, which later kits add (GetStats and CreateQueue1,
// EnqueueSetEvent and Query1: Microsoft's DirectStorage reference for the GDK). Their IIDs are
// the ones a title built with such a kit asks for: the first of DStorageGetFactory, on whose
// answer it calls the eleventh method with a DSTORAGE_QUEUE_DESC1 and the second.
constexpr GUID factory2_iid{
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID queue2_iid{
    0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};

template <class Interface> class Object : public Interface {
  public:
    Object(std::initializer_list<GUID> iids) : iids_(iids) {}
    virtual ~Object() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid != IID_IUnknown && std::ranges::find(iids_, iid) == iids_.end()) {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        *object = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }

  private:
    std::vector<GUID> iids_;
    std::atomic<ULONG> references_{1};
};

// the interfaces, with their methods in the order the console's header has them
struct FileInterface : IUnknown {
    virtual void Close() = 0;
    virtual HANDLE GetHandle() = 0;
};
struct StatusInterface : IUnknown {
    virtual bool IsComplete(UINT32 index) = 0;
    virtual HRESULT GetHResult(UINT32 index) = 0;
};
struct QueueDesc {
    UINT64 source_type;
    UINT16 capacity;
    INT8 priority;
    const char *name;
};
struct QueueInfo {
    QueueDesc desc;
    UINT16 empty_slots, until_submit;
};
struct QueueDesc1 : QueueDesc {
    UINT64 options; // bit 0: the queue does not submit itself
};
struct QueueInfo1 {
    QueueDesc1 desc;
    UINT16 empty_slots, until_submit;
};
static_assert(sizeof(QueueDesc) == 24 && offsetof(QueueDesc1, options) == 24);
// DSTORAGE_REQUEST_OPTIONS and DSTORAGE_REQUEST, with the members Microsoft's DirectStorage
// reference for the GDK gives them
struct RequestOptions {
    UINT64 zlib_decompress : 1; // RFC 1950
    UINT64 bcpack_mode : 4;     // DSTORAGE_BCPACK_MODE: 0 none, else the BC number
    UINT64 swizzle_mode : 16;
    UINT64 destination_is_physical_pages : 1;
    UINT64 source_type : 1;              // DSTORAGE_REQUEST_SOURCE_FILE (0) or _MEMORY (1)
    UINT64 source_is_physical_pages : 1; // with a memory source
    UINT64 reserved : 40;
};
constexpr UINT64 source_memory = 1;
struct Request {
    RequestOptions options;
    // Destination, or DestinationPageArray and DestinationPageOffset: a page number per 64 KiB
    // and where in the first page the data starts
    union {
        void *destination;
        const ULONG_PTR *destination_page_array;
    };
    UINT64 destination_page_offset; // UINT16, in a union as large as two pointers
    UINT32 destination_size;
    // File and FileOffset, Source, or SourcePageArray and SourcePageOffset
    union {
        void *source;
        const ULONG_PTR *source_page_array;
    };
    UINT64 file_offset; // or SourcePageOffset (UINT16)
    UINT32 source_size, intermediate_size;
    UINT64 tag; // CancellationTag
    const char *name;
};
static_assert(sizeof(RequestOptions) == 8 && offsetof(Request, destination_size) == 24 &&
              offsetof(Request, source) == 32 && offsetof(Request, source_size) == 48 &&
              offsetof(Request, tag) == 56);
// a request from a file holds the file (IDStorageFileX) until it is done
IUnknown *file_of(const Request &request) {
    return request.options.source_type == source_memory ? nullptr
                                                        : static_cast<IUnknown *>(request.source);
}
struct ErrorRecord {
    UINT32 failures;
    HRESULT first;
    std::uint8_t rest[1024];
};
struct QueueInterface : IUnknown {
    virtual void EnqueueRequest(const Request *request) = 0;
    virtual void EnqueueStatus(StatusInterface *status, UINT32 index) = 0;
    virtual void EnqueueSignal(ID3D12Fence *fence, UINT64 value) = 0;
    virtual void Submit() = 0;
    virtual void CancelRequestsWithTag(UINT64 mask, UINT64 value) = 0;
    virtual void Close() = 0;
    virtual HANDLE GetErrorEvent() = 0;
    virtual void RetrieveErrorRecord(ErrorRecord *record) = 0;
    virtual void Query(QueueInfo *info) = 0;
    virtual void EnqueueSetEvent(HANDLE event) = 0;
    virtual void Query1(QueueInfo1 *info) = 0;
};
struct FactoryInterface : IUnknown {
    virtual HRESULT CreateQueue(const QueueDesc *desc, REFIID iid, void **queue) = 0;
    virtual HRESULT OpenFile(const WCHAR *path, REFIID iid, void **file) = 0;
    virtual HRESULT CreateStatusArray(UINT32 capacity, const char *name, REFIID iid,
                                      void **status) = 0;
    virtual HRESULT SetCpuAffinity(UINT64 affinity) = 0;
    virtual void SetDebugFlags(UINT32 flags) = 0;
    virtual HRESULT SetStagingBufferSize(UINT32 size) = 0;
    virtual void GetStats(void *stats) = 0;
    virtual HRESULT CreateQueue1(const QueueDesc1 *desc, REFIID iid, void **queue) = 0;
};

class File final : public Object<FileInterface> {
  public:
    File(HANDLE handle, HANDLE overlapped)
        : Object({file_iid}), handle_(handle), overlapped_(overlapped) {}
    ~File() override { Close(); }
    void Close() override {
        for (auto *const kept : {&handle_, &overlapped_})
            if (const auto handle = kept->exchange(INVALID_HANDLE_VALUE);
                handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
    }
    HANDLE GetHandle() override { return handle_; }
    // The same file opened for overlapped reads, which the queues read it through: the host
    // does one read at a time of a handle that is not, and a queue reads many at once.
    HANDLE overlapped() const {
        const auto handle = overlapped_.load();
        return handle != INVALID_HANDLE_VALUE ? handle : handle_.load();
    }

  private:
    std::atomic<HANDLE> handle_, overlapped_;
};

class Status final : public Object<StatusInterface> {
  public:
    explicit Status(UINT32 capacity) : Object({status_iid}), entries_(capacity) {}
    bool IsComplete(UINT32 index) override {
        const std::lock_guard lock(mutex_);
        return index < entries_.size() && entries_[index].done;
    }
    HRESULT GetHResult(UINT32 index) override {
        const std::lock_guard lock(mutex_);
        if (index >= entries_.size())
            return E_INVALIDARG;
        return entries_[index].done ? entries_[index].result : E_PENDING;
    }
    void set(UINT32 index, HRESULT result) {
        const std::lock_guard lock(mutex_);
        if (index < entries_.size())
            entries_[index] = {true, result};
    }
    void pending(UINT32 index) {
        const std::lock_guard lock(mutex_);
        if (index < entries_.size())
            entries_[index] = {};
    }

  private:
    struct Entry {
        bool done = false;
        HRESULT result = S_OK;
    };
    std::mutex mutex_;
    std::vector<Entry> entries_;
};

HRESULT read_file(const Request &request, void *target) {
    // an event of the thread's own, for its reads to wait on
    thread_local HANDLE event = nullptr;
    if (!event)
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(request.file_offset);
    at.OffsetHigh = static_cast<DWORD>(request.file_offset >> 32);
    at.hEvent = event;
    const auto handle =
        static_cast<File *>(static_cast<FileInterface *>(request.source))->overlapped();
    DWORD done = 0;
    SetLastError(ERROR_SUCCESS);
    if (!ReadFile(handle, target, request.source_size, &done, &at) &&
        (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(handle, &at, &done, TRUE)))
        return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_HANDLE_EOF);
    if (done != request.source_size)
        return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
    return S_OK;
}
// where a request's data comes from: a file, the title's memory or its physical pages
HRESULT read_source(const Request &request, std::vector<std::uint8_t> &buffer,
                    const std::uint8_t *&data) {
    if (request.options.source_type != source_memory) {
        buffer.resize(request.source_size);
        data = buffer.data();
        return read_file(request, buffer.data());
    }
    if (request.options.source_is_physical_pages) {
        buffer.resize(request.source_size);
        data = buffer.data();
        return copy_physical_pages(request.source_page_array, request.file_offset & 0xffff,
                                   buffer.data(), buffer.size(), false)
                   ? S_OK
                   : E_INVALIDARG;
    }
    data = static_cast<const std::uint8_t *>(request.source);
    return S_OK;
}
// where it goes: the title's memory or its physical pages
HRESULT write_destination(const Request &request, const std::uint8_t *data, std::size_t size) {
    if (request.options.destination_is_physical_pages)
        return copy_physical_pages(request.destination_page_array,
                                   request.destination_page_offset & 0xffff,
                                   const_cast<std::uint8_t *>(data), size, true)
                   ? S_OK
                   : E_INVALIDARG;
    prepare_title_write(request.destination, size);
    std::memcpy(request.destination, data, size);
    return S_OK;
}

// BCPack's tables are the format's constants, built into the runtime. Tests may replace them
// through use_bcpack_tables.
struct BcpackTables {
    std::mutex mutex;
    bool looked = false;
    std::shared_ptr<const bcpack::Tables> tables;
};
BcpackTables &bcpack_state() {
    static BcpackTables state;
    return state;
}
std::shared_ptr<const bcpack::Tables> bcpack_tables() {
    auto &state = bcpack_state();
    const std::lock_guard lock(state.mutex);
    if (!state.looked) {
        state.looked = true;
        std::string why;
        state.tables = bcpack::parse_tables(bcpack::builtin_tables(), &why);
        if (!state.tables)
            std::fprintf(stderr,
                         "[Runtime] DirectStorage: the built-in BCPack tables are invalid: %s\n",
                         why.c_str());
    }
    return state.tables;
}

// The swizzle mode is not done here: the data is written as it was read.
HRESULT read(const Request &request) {
    const auto &options = request.options;
    const auto bc = static_cast<unsigned>(options.bcpack_mode);
    std::shared_ptr<const bcpack::Tables> tables;
    if (bc && (bc == 2 || !(tables = bcpack_tables())))
        return E_NOTIMPL;
    if (!request.destination || !request.source)
        return E_INVALIDARG;
    // without decompression the sizes are the same; with it the result is larger
    const bool decompress = options.zlib_decompress || bc;
    if (!decompress && request.source_size > request.destination_size)
        return E_INVALIDARG;
    // a file read straight to the title's memory, when nothing comes in between
    if (!decompress && options.source_type != source_memory &&
        !options.destination_is_physical_pages) {
        const TitleWrite writing(request.destination, request.source_size);
        return read_file(request, request.destination);
    }
    std::vector<std::uint8_t> packed;
    const std::uint8_t *data = nullptr;
    if (const auto result = read_source(request, packed, data); FAILED(result))
        return result;
    std::size_t size = request.source_size;
    // zlib first, to IntermediateSize when BCPack follows, else to DestinationSize. A stream
    // that goes on past that fills it: the console's decompression stops where its output
    // ends, and a title asks for the start of a longer stream that way.
    std::vector<std::uint8_t> plain;
    if (options.zlib_decompress) {
        const std::size_t room =
            bc && request.intermediate_size ? request.intermediate_size : request.destination_size;
        if (!inflate_zlib({data, size}, plain))
            return E_FAIL;
        data = plain.data();
        size = std::min(plain.size(), room);
    }
    // then BCPack, to the destination's blocks
    std::vector<std::uint8_t> blocks;
    if (bc) {
        blocks.resize(request.destination_size);
        if (std::string why; !bcpack::decode(*tables, bc, {data, size}, blocks, &why)) {
            if (static std::atomic<int> shown{0}; shown++ < 4)
                std::fprintf(stderr, "[Runtime] DirectStorage: BCPack (BC%u) of %zu bytes: %s\n",
                             bc, size, why.c_str());
            return E_FAIL;
        }
        data = blocks.data();
        size = blocks.size();
    }
    return write_destination(request, data, size);
}

class Queue final : public Object<QueueInterface> {
  public:
    explicit Queue(const QueueDesc1 &desc)
        : Object({queue_iid, queue2_iid}), desc_(desc), name_(desc.name ? desc.name : ""),
          error_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        desc_.name = name_.c_str();
    }
    ~Queue() override {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        if (worker_.joinable())
            worker_.join();
        CloseHandle(error_);
    }
    void EnqueueRequest(const Request *request) override {
        if (!request)
            return;
        {
            const std::lock_guard lock(mutex_);
            // the file is held until the request is done
            if (auto *const file = file_of(*request))
                file->AddRef();
            pending_.emplace_back(*request);
        }
        submit_when_full();
    }
    void EnqueueStatus(StatusInterface *status, UINT32 index) override {
        if (!status)
            return;
        status->AddRef();
        static_cast<Status *>(status)->pending(index);
        {
            const std::lock_guard lock(mutex_);
            pending_.emplace_back(Mark{static_cast<Status *>(status), index});
        }
        submit_when_full();
    }
    void EnqueueSignal(ID3D12Fence *fence, UINT64 value) override {
        if (!fence)
            return;
        fence->AddRef();
        {
            const std::lock_guard lock(mutex_);
            pending_.emplace_back(Signal{fence, value});
        }
        submit_when_full();
    }
    void EnqueueSetEvent(HANDLE event) override {
        if (!event)
            return;
        {
            const std::lock_guard lock(mutex_);
            pending_.emplace_back(Event{event});
        }
        submit_when_full();
    }
    // As on the console, the submitting thread goes on at once: a title submits from threads
    // that have more to do, and one may watch that they do (a title stops itself when one of
    // them has not got on for a while). The queue's thread does the submissions in order.
    void Submit() override {
        {
            const std::lock_guard lock(mutex_);
            if (pending_.empty())
                return;
            submitted_.push_back(std::move(pending_));
            pending_.clear();
            if (!worker_.joinable())
                worker_ = std::thread([this] { work(); });
        }
        ready_.notify_one();
    }
    void CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        const auto cancelled = [&](Entry &entry) {
            auto *const request = std::get_if<Request>(&entry);
            if (!request || (request->tag & mask) != value)
                return false;
            if (auto *const file = file_of(*request))
                file->Release();
            return true;
        };
        const std::lock_guard lock(mutex_);
        std::erase_if(pending_, cancelled);
        for (auto &work : submitted_)
            std::erase_if(work, cancelled);
    }
    // what was enqueued is done before the queue is
    void Close() override {
        Submit();
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [&] { return submitted_.empty() && !busy_; });
    }
    HANDLE GetErrorEvent() override { return error_; }
    void RetrieveErrorRecord(ErrorRecord *record) override {
        if (!record)
            return;
        const std::lock_guard lock(mutex_);
        *record = {};
        record->failures = failures_;
        record->first = first_failure_;
        failures_ = 0;
        first_failure_ = S_OK;
        ResetEvent(error_);
    }
    void Query(QueueInfo *info) override {
        if (!info)
            return;
        QueueInfo1 all;
        Query1(&all);
        *info = {all.desc, all.empty_slots, all.until_submit};
    }
    void Query1(QueueInfo1 *info) override {
        if (!info)
            return;
        const std::lock_guard lock(mutex_);
        const auto used =
            static_cast<UINT16>(std::min<std::size_t>(pending_.size(), desc_.capacity));
        *info = {desc_, static_cast<UINT16>(desc_.capacity - used),
                 static_cast<UINT16>(desc_.capacity / 2 > used ? desc_.capacity / 2 - used : 0)};
    }

  private:
    struct Mark {
        Status *status;
        UINT32 index;
    };
    struct Signal {
        ID3D12Fence *fence;
        UINT64 value;
    };
    struct Event {
        HANDLE handle;
    };
    using Entry = std::variant<Request, Mark, Signal, Event>;
    void work() {
        for (;;) {
            std::vector<Entry> entries;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !submitted_.empty(); });
                if (submitted_.empty())
                    return; // stopping, with nothing left to do
                entries = std::move(submitted_.front());
                submitted_.pop_front();
                busy_ = true;
            }
            run(entries);
            {
                const std::lock_guard lock(mutex_);
                busy_ = false;
            }
            idle_.notify_all();
        }
    }
    // The requests up to each mark are read at once, the marks after them in order.
    void run(std::vector<Entry> &entries) {
        // XV_DS_TIME: when each submission starts, how much it reads and how long it takes
        const auto started = GetTickCount64();
        std::uint64_t requests = 0, packed = 0, plain = 0, first_offset = 0;
        for (const auto &entry : entries)
            if (const auto *const request = std::get_if<Request>(&entry)) {
                if (!requests++)
                    first_offset = request->file_offset;
                packed += request->source_size;
                plain += request->destination_size;
            }
        HRESULT since_mark = S_OK;
        for (std::size_t at = 0; at < entries.size();) {
            auto end = at;
            while (end < entries.size() && std::holds_alternative<Request>(entries[end]))
                ++end;
            if (end > at) {
                std::mutex failing;
                std::for_each(std::execution::par, entries.begin() + at, entries.begin() + end,
                              [&](Entry &entry) {
                                  const auto result = finish(std::get<Request>(entry));
                                  if (SUCCEEDED(result))
                                      return;
                                  const std::lock_guard lock(failing);
                                  if (SUCCEEDED(since_mark))
                                      since_mark = result;
                              });
                at = end;
                continue;
            }
            auto &entry = entries[at++];
            if (auto *const mark = std::get_if<Mark>(&entry)) {
                mark->status->set(mark->index, since_mark);
                mark->status->Release();
                since_mark = S_OK;
            } else if (auto *const signal = std::get_if<Signal>(&entry)) {
                signal->fence->Signal(signal->value);
                signal->fence->Release();
            } else if (auto *const event = std::get_if<Event>(&entry)) {
                SetEvent(event->handle);
            }
        }
        if (std::getenv("XV_DS_TIME")) {
            std::fprintf(stderr,
                         "[Time] at %llu ms DirectStorage submit on %s: %zu entries, %llu requests "
                         "from offset %llu, %llu -> %llu bytes, %llu ms\n",
                         started, name_.c_str(), entries.size(), requests, first_offset, packed,
                         plain, GetTickCount64() - started);
            std::fflush(stderr);
        }
    }
    // a request read, its file let go of and a failure kept for the error record
    HRESULT finish(const Request &request) {
        const auto result = read(request);
        // XV_FILE_TRACE: the first requests, and every one that fails
        if (static std::atomic<int> shown{0};
            std::getenv("XV_FILE_TRACE") && (FAILED(result) || shown < 24) && shown++ < 200)
            std::fprintf(stderr,
                         "[Runtime] DirectStorage request: options 0x%llx, %u bytes at %llu to %u "
                         "bytes (intermediate %u): 0x%08lx\n",
                         std::bit_cast<UINT64>(request.options), request.source_size,
                         request.file_offset, request.destination_size, request.intermediate_size,
                         static_cast<unsigned long>(result));
        if (auto *const file = file_of(request))
            file->Release();
        if (FAILED(result)) {
            const std::lock_guard lock(mutex_);
            if (!failures_++)
                first_failure_ = result;
            SetEvent(error_);
        }
        return result;
    }
    // a queue submits itself when half of it is taken, unless it was made not to
    void submit_when_full() {
        if (desc_.options & 1)
            return;
        bool full;
        {
            const std::lock_guard lock(mutex_);
            full = pending_.size() >= std::max<std::size_t>(desc_.capacity / 2, 1);
        }
        if (full)
            Submit();
    }
    QueueDesc1 desc_;
    std::string name_;
    HANDLE error_;
    std::mutex mutex_;
    std::condition_variable ready_, idle_;
    std::vector<Entry> pending_;
    std::deque<std::vector<Entry>> submitted_; // for the queue's thread, in order
    bool busy_ = false, stopping_ = false;
    std::thread worker_;
    UINT32 failures_ = 0;
    HRESULT first_failure_ = S_OK;
};

class Factory final : public Object<FactoryInterface> {
  public:
    Factory() : Object({factory_iid, factory2_iid}) {}
    HRESULT CreateQueue(const QueueDesc *desc, REFIID iid, void **queue) override {
        if (!desc)
            return E_POINTER;
        const QueueDesc1 with_options{*desc, 0};
        return CreateQueue1(&with_options, iid, queue);
    }
    // the structure's priority records are not known here, so nothing is written
    void GetStats(void *) override {}
    HRESULT CreateQueue1(const QueueDesc1 *desc, REFIID iid, void **queue) override {
        if (!desc || !queue)
            return E_POINTER;
        auto *const made = new Queue(*desc);
        const auto result = made->QueryInterface(iid, queue);
        made->Release();
        return result;
    }
    HRESULT OpenFile(const WCHAR *path, REFIID iid, void **file) override {
        if (!path || !file)
            return E_POINTER;
        *file = nullptr;
        const auto handle =
            CreateFileW(console_file_path(path).c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                        nullptr); // a title opens the root of its package as well
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (static std::atomic<int> shown{0}; shown++ < 6)
                std::fprintf(stderr,
                             "[Runtime] DirectStorage: cannot open %ls (as %ls): error %lu\n", path,
                             console_file_path(path).c_str(), error);
            return HRESULT_FROM_WIN32(error);
        }
        if (static std::atomic<int> shown{0}; std::getenv("XV_FILE_TRACE") && shown++ < 40)
            std::fprintf(stderr, "[Runtime] DirectStorage: opened %ls (%zu characters) from %s\n",
                         path, wcslen(path), detail::guest_location(_ReturnAddress()).c_str());
        auto *const made =
            new File(handle, CreateFileW(console_file_path(path).c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS |
                                             FILE_FLAG_OVERLAPPED,
                                         nullptr));
        const auto result = made->QueryInterface(iid, file);
        made->Release();
        return result;
    }
    HRESULT CreateStatusArray(UINT32 capacity, const char *, REFIID iid, void **status) override {
        if (!status)
            return E_POINTER;
        auto *const made = new Status(capacity);
        const auto result = made->QueryInterface(iid, status);
        made->Release();
        return result;
    }
    HRESULT SetCpuAffinity(UINT64) override { return S_OK; }
    void SetDebugFlags(UINT32) override {}
    HRESULT SetStagingBufferSize(UINT32) override { return S_OK; }
};

HRESULT WINAPI get_factory(const GUID *iid, void **factory) {
    if (!iid || !factory)
        return E_POINTER;
    static Factory *const one = new Factory;
    return one->QueryInterface(*iid, factory);
}
} // namespace

bool use_bcpack_tables(std::span<const std::uint8_t> bytes) {
    auto tables = bcpack::parse_tables(bytes, nullptr);
    auto &state = bcpack_state();
    const std::lock_guard lock(state.mutex);
    state.looked = true;
    state.tables = std::move(tables);
    return state.tables != nullptr;
}

void install_dstorage(Registry &registry) {
    for (const auto *module : {"dstorage_xs.dll", "xcrdapi.dll"})
        registry.add(module, "DStorageGetFactory", reinterpret_cast<void *>(&get_factory));
}
} // namespace xv::runtime::modules
