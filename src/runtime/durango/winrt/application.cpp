#include "runtime/durango/winrt/application.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/durango/winrt/collection.hpp"
#include "runtime/durango/winrt/object.hpp"
#include "runtime/modules/watchdog.hpp"
#include <atomic>
#include <corewindow.h>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <vector>
#include <weakreference.h>
#include <windows.applicationmodel.activation.h>
#include <windows.applicationmodel.core.h>
#include <windows.foundation.h>
#include <windows.ui.core.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

namespace xv::runtime::durango::winrt {
using xv::surface::TitleWindow;
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::EventSource;
using Microsoft::WRL::Make;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::ApplicationModel;
using namespace ABI::Windows::ApplicationModel::Activation;
using namespace ABI::Windows::ApplicationModel::Core;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::UI::Core;
namespace Collections = ABI::Windows::Foundation::Collections;

// what a console reports whatever the host window measures
constexpr Rect bounds{0, 0, 1920, 1080};

// The action RunAsync returns: the handler has already run by then.
class CompletedAction : public Class<CompletedAction, IAsyncAction, IAsyncInfo> {
  public:
    static constexpr wchar_t name[] = L"Windows.Foundation.IAsyncAction";
    STDMETHODIMP put_Completed(IAsyncActionCompletedHandler *handler) override {
        return handler ? handler->Invoke(this, AsyncStatus::Completed) : S_OK;
    }
    STDMETHODIMP get_Completed(IAsyncActionCompletedHandler **handler) override {
        *handler = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetResults() override { return S_OK; }
    STDMETHODIMP get_Id(unsigned *id) override {
        *id = 1;
        return S_OK;
    }
    STDMETHODIMP get_Status(AsyncStatus *status) override {
        *status = AsyncStatus::Completed;
        return S_OK;
    }
    STDMETHODIMP get_ErrorCode(HRESULT *code) override {
        *code = S_OK;
        return S_OK;
    }
    STDMETHODIMP Cancel() override { return S_OK; }
    STDMETHODIMP Close() override { return S_OK; }
};

// The arguments of the window events the shell raises when the title's window comes up
class ActivatedArgs : public Class<ActivatedArgs, IWindowActivatedEventArgs, ICoreWindowEventArgs> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.WindowActivatedEventArgs";
    STDMETHODIMP get_WindowActivationState(CoreWindowActivationState *value) override {
        *value = CoreWindowActivationState_CodeActivated;
        return S_OK;
    }
    STDMETHODIMP get_Handled(boolean *value) override {
        *value = handled_;
        return S_OK;
    }
    STDMETHODIMP put_Handled(boolean value) override {
        handled_ = value;
        return S_OK;
    }

  private:
    boolean handled_ = false;
};
class VisibilityArgs
    : public Class<VisibilityArgs, IVisibilityChangedEventArgs, ICoreWindowEventArgs> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.VisibilityChangedEventArgs";
    STDMETHODIMP get_Visible(boolean *value) override {
        *value = true;
        return S_OK;
    }
    STDMETHODIMP get_Handled(boolean *value) override {
        *value = handled_;
        return S_OK;
    }
    STDMETHODIMP put_Handled(boolean value) override {
        handled_ = value;
        return S_OK;
    }

  private:
    boolean handled_ = false;
};

class Dispatcher : public Class<Dispatcher, ICoreDispatcher> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.CoreDispatcher";
    explicit Dispatcher(TitleWindow &window) : window_(window), thread_(GetCurrentThreadId()) {}
    // what ProcessEvents runs next on the thread that calls it: window events
    void post(std::function<void()> work) {
        const std::lock_guard lock(mutex_);
        pending_.push_back(std::move(work));
    }
    STDMETHODIMP get_HasThreadAccess(boolean *value) override {
        *value = GetCurrentThreadId() == thread_;
        return S_OK;
    }
    STDMETHODIMP ProcessEvents(CoreProcessEventsOption options) override {
        // The window's messages are handled on another thread; what there is to process
        // here are the events raised for the title, then only the options that wait wait.
        std::vector<std::function<void()>> work;
        {
            const std::lock_guard lock(mutex_);
            work.swap(pending_);
        }
        for (auto &item : work)
            item();
        if (options == CoreProcessEventsOption_ProcessUntilQuit)
            window_.wait_closed();
        return S_OK;
    }
    // Runs immediately on the dispatcher thread, or queues for ProcessEvents from worker threads.
    STDMETHODIMP RunAsync(CoreDispatcherPriority, IDispatchedHandler *handler,
                          IAsyncAction **action) override {
        if (!handler || !action)
            return E_POINTER;
        if (GetCurrentThreadId() != thread_) {
            ComPtr<IDispatchedHandler> held(handler);
            post([held] { held->Invoke(); });
            return Make<CompletedAction>().CopyTo(action);
        }
        const auto result = handler->Invoke();
        if (FAILED(result))
            return result;
        return Make<CompletedAction>().CopyTo(action);
    }
    STDMETHODIMP RunIdleAsync(IIdleDispatchedHandler *, IAsyncAction **action) override {
        *action = nullptr;
        return E_NOTIMPL;
    }

  private:
    TitleWindow &window_;
    DWORD thread_;
    std::mutex mutex_;
    std::vector<std::function<void()>> pending_;
};

// ICoreWindow as the console has it, which is not the desktop one: the bounds are the first
// method, the dispatcher the second and Activate the sixth. Events follow the key states in
// pairs, add then remove: Activated (15), CharacterReceived, Closed, InputEnabled, KeyDown,
// KeyUp, PointerMoved, PointerEntered, PointerExited, SizeChanged and VisibilityChanged (35).
// What comes after those is not known yet.
struct IConsoleWindow : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Bounds(Rect *value) = 0;                 // 6
    virtual HRESULT STDMETHODCALLTYPE get_Dispatcher(ICoreDispatcher **value) = 0; // 7
    virtual HRESULT STDMETHODCALLTYPE get_IsInputEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsInputEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Visible(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE Activate() = 0; // 11
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAsyncKeyState(int key, CoreVirtualKeyStates *state) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetKeyState(int key, CoreVirtualKeyStates *state) = 0;
    // clang-format off
#define XV_UNKNOWN(Index) virtual HRESULT STDMETHODCALLTYPE unknown##Index(void *, void *) = 0;
    XV_UNKNOWN(15) XV_UNKNOWN(16) XV_UNKNOWN(17)
    XV_UNKNOWN(18) XV_UNKNOWN(19) XV_UNKNOWN(20) XV_UNKNOWN(21) XV_UNKNOWN(22) XV_UNKNOWN(23)
    XV_UNKNOWN(24) XV_UNKNOWN(25) XV_UNKNOWN(26) XV_UNKNOWN(27) XV_UNKNOWN(28) XV_UNKNOWN(29)
    XV_UNKNOWN(30) XV_UNKNOWN(31) XV_UNKNOWN(32) XV_UNKNOWN(33) XV_UNKNOWN(34) XV_UNKNOWN(35)
    XV_UNKNOWN(36) XV_UNKNOWN(37) XV_UNKNOWN(38) XV_UNKNOWN(39) XV_UNKNOWN(40) XV_UNKNOWN(41)
    XV_UNKNOWN(42) XV_UNKNOWN(43) XV_UNKNOWN(44) XV_UNKNOWN(45) XV_UNKNOWN(46) XV_UNKNOWN(47)
#undef XV_UNKNOWN
    // clang-format on
};

class Window final : public IConsoleWindow {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.CoreWindow";
    Window() : host_(L"XValkyrie", 1280, 720), dispatcher_(Make<Dispatcher>(host_)) {}
    TitleWindow &host() { return host_; }
    Dispatcher &dispatcher() { return *dispatcher_.Get(); }

    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) ||
            iid == __uuidof(IAgileObject) || iid == __uuidof(ICoreWindow)) {
            *object = static_cast<IConsoleWindow *>(this);
            AddRef();
            return S_OK;
        }
        if (iid == __uuidof(ICoreWindowInterop)) {
            *object = &interop_;
            AddRef();
            return S_OK;
        }
        if (iid == __uuidof(IWeakReferenceSource))
            return weak_reference_source(static_cast<IConsoleWindow *>(this), object);
        return placeholder_interface(name, iid, object);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }
    STDMETHODIMP GetIids(ULONG *count, IID **iids) override {
        *count = 0;
        *iids = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetRuntimeClassName(HSTRING *value) override {
        return WindowsCreateString(name, static_cast<UINT32>(wcslen(name)), value);
    }
    STDMETHODIMP GetTrustLevel(TrustLevel *level) override {
        *level = BaseTrust;
        return S_OK;
    }
    STDMETHODIMP get_Bounds(Rect *value) override {
        *value = bounds;
        return S_OK;
    }
    STDMETHODIMP get_Dispatcher(ICoreDispatcher **value) override {
        return dispatcher_.CopyTo(value);
    }
    // The host window stays hidden until the title presents its first frame.
    STDMETHODIMP Activate() override { return S_OK; }
    STDMETHODIMP get_IsInputEnabled(boolean *value) override {
        *value = true;
        return S_OK;
    }
    STDMETHODIMP put_IsInputEnabled(boolean) override { return S_OK; }
    STDMETHODIMP get_Visible(boolean *value) override {
        *value = true;
        return S_OK;
    }
    STDMETHODIMP Close() override { return S_OK; }
    // the host's keyboard, while the title's window is the one typed into
    STDMETHODIMP GetAsyncKeyState(int key, CoreVirtualKeyStates *state) override {
        if (!state)
            return E_POINTER;
        *state = CoreVirtualKeyStates_None;
        if (GetForegroundWindow() != static_cast<HWND>(host_.native()))
            return S_OK;
        if (::GetAsyncKeyState(key) & 0x8000)
            *state = CoreVirtualKeyStates_Down;
        if (::GetKeyState(key) & 1)
            *state = static_cast<CoreVirtualKeyStates>(*state | CoreVirtualKeyStates_Locked);
        return S_OK;
    }
    STDMETHODIMP GetKeyState(int key, CoreVirtualKeyStates *state) override {
        return GetAsyncKeyState(key, state);
    }
    // clang-format off
#define XV_UNKNOWN(Index)                                                                          \
    STDMETHODIMP unknown##Index(void *first, void *second) override {                              \
        return unknown_method(name, "console ICoreWindow", Index, first, second,                   \
                              _ReturnAddress());                                                   \
    }
    XV_UNKNOWN(38) XV_UNKNOWN(39)
    XV_UNKNOWN(41) XV_UNKNOWN(43) XV_UNKNOWN(44) XV_UNKNOWN(45)
    XV_UNKNOWN(46) XV_UNKNOWN(47)
#undef XV_UNKNOWN
#define XV_REMOVE(Index) STDMETHODIMP unknown##Index(void *, void *) override { return S_OK; }
    XV_REMOVE(16) XV_REMOVE(18) XV_REMOVE(20) XV_REMOVE(22) XV_REMOVE(24) XV_REMOVE(26)
    XV_REMOVE(28) XV_REMOVE(30) XV_REMOVE(32) XV_REMOVE(34) XV_REMOVE(36)
#undef XV_REMOVE
    // Event registrations (handler, token) that nothing raises: there is no keyboard event,
    // pointer or resize to tell a title of
#define XV_EVENT(Index)                                                                            \
    STDMETHODIMP unknown##Index(void *, void *token) override {                                    \
        *static_cast<std::int64_t *>(token) = ++tokens_;                                           \
        return S_OK;                                                                               \
    }
    XV_EVENT(17) XV_EVENT(19) XV_EVENT(21) XV_EVENT(23) XV_EVENT(25) XV_EVENT(27) XV_EVENT(29)
    XV_EVENT(31) XV_EVENT(33) XV_EVENT(37)
    XV_EVENT(40) XV_EVENT(42)
#undef XV_EVENT
        // clang-format on
        // Activated (15) and VisibilityChanged (35), which the shell raises once the window is
        // up: activated, and visible. Titles wait for them before they start.
        STDMETHODIMP unknown15(void *handler, void *token) override {
        return raise_once<ActivatedArgs>(handler, token);
    }
    STDMETHODIMP unknown35(void *handler, void *token) override {
        return raise_once<VisibilityArgs>(handler, token);
    }
    template <class Args> HRESULT raise_once(void *handler, void *token) {
        if (!handler || !token)
            return E_POINTER;
        *static_cast<std::int64_t *>(token) = ++tokens_;
        ComPtr<IUnknown> held(static_cast<IUnknown *>(handler));
        ComPtr<Window> self(this);
        dispatcher_->post([held, self] {
            // ITypedEventHandler<CoreWindow *, Args *>::Invoke(sender, args), slot 3
            using Invoke = HRESULT(STDMETHODCALLTYPE *)(IUnknown *, IInspectable *, IInspectable *);
            auto args = Make<Args>();
            (*reinterpret_cast<Invoke **>(held.Get()))[3](
                held.Get(),
                reinterpret_cast<IInspectable *>(static_cast<IConsoleWindow *>(self.Get())),
                reinterpret_cast<IInspectable *>(args.Get()));
        });
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1};
    std::atomic<std::int64_t> tokens_{0};
    TitleWindow host_;
    ComPtr<Dispatcher> dispatcher_;
    // public ICoreWindowInterop: the HWND, which engines such as Unity draw into
    struct Interop final : ICoreWindowInterop {
        Window *window;
        explicit Interop(Window *owner) : window(owner) {}
        STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
            return window->QueryInterface(iid, object);
        }
        STDMETHODIMP_(ULONG) AddRef() override { return window->AddRef(); }
        STDMETHODIMP_(ULONG) Release() override { return window->Release(); }
        STDMETHODIMP get_WindowHandle(HWND *handle) override {
            if (!handle)
                return E_POINTER;
            *handle = static_cast<HWND>(window->host_.native());
            return S_OK;
        }
        STDMETHODIMP put_MessageHandled(boolean) override { return S_OK; }
    } interop_{this};
};

// the window of the one view, once CoreApplication::Run has created it
Window *current_window = nullptr;

class WindowStatics : public Class<WindowStatics, IActivationFactory, ICoreWindowStatic> {
  public:
    static constexpr wchar_t name[] = L"Windows.UI.Core.CoreWindow";
    STDMETHODIMP ActivateInstance(IInspectable **instance) override {
        *instance = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetForCurrentThread(ICoreWindow **window) override {
        *window = reinterpret_cast<ICoreWindow *>(static_cast<IConsoleWindow *>(current_window));
        if (current_window)
            current_window->AddRef();
        return S_OK;
    }
};

class LaunchArguments
    : public Class<LaunchArguments, ILaunchActivatedEventArgs, IActivatedEventArgs> {
  public:
    static constexpr wchar_t name[] =
        L"Windows.ApplicationModel.Activation.LaunchActivatedEventArgs";
    STDMETHODIMP get_Kind(ActivationKind *value) override {
        *value = ActivationKind_Launch;
        return S_OK;
    }
    STDMETHODIMP get_PreviousExecutionState(ApplicationExecutionState *value) override {
        *value = ApplicationExecutionState_NotRunning;
        return S_OK;
    }
    STDMETHODIMP get_SplashScreen(ISplashScreen **value) override {
        *value = nullptr;
        return S_OK;
    }
    STDMETHODIMP get_Arguments(HSTRING *value) override {
        *value = nullptr;
        return S_OK;
    }
    STDMETHODIMP get_TileId(HSTRING *value) override {
        return WindowsCreateString(L"App", 3, value);
    }
};

class View : public Class<View, ICoreApplicationView> {
  public:
    static constexpr wchar_t name[] = L"Windows.ApplicationModel.Core.CoreApplicationView";
    View() { current_window = window_.Get(); }
    STDMETHODIMP get_CoreWindow(ICoreWindow **value) override {
        *value = reinterpret_cast<ICoreWindow *>(static_cast<IConsoleWindow *>(window_.Get()));
        window_->AddRef();
        return S_OK;
    }
    STDMETHODIMP
    add_Activated(ITypedEventHandler<CoreApplicationView *, IActivatedEventArgs *> *handler,
                  EventRegistrationToken *token) override {
        const auto result = activated_.Add(handler, token);
        // the shell raises Activated through the dispatcher: a title that subscribes only in
        // Run gets it at its next ProcessEvents
        if (SUCCEEDED(result) && activated_once_ && handler) {
            ComPtr<ITypedEventHandler<CoreApplicationView *, IActivatedEventArgs *>> held(handler);
            ComPtr<View> self(this);
            window_->dispatcher().post([held, self] {
                const auto arguments = Make<LaunchArguments>();
                held->Invoke(reinterpret_cast<ICoreApplicationView *>(self.Get()),
                             static_cast<IActivatedEventArgs *>(arguments.Get()));
            });
        }
        return result;
    }
    STDMETHODIMP remove_Activated(EventRegistrationToken token) override {
        return activated_.Remove(token);
    }
    STDMETHODIMP get_IsMain(boolean *value) override {
        *value = true;
        return S_OK;
    }
    STDMETHODIMP get_IsHosted(boolean *value) override {
        *value = false;
        return S_OK;
    }
    HRESULT activate() {
        activated_once_ = true;
        const auto arguments = Make<LaunchArguments>();
        return activated_.InvokeAll(reinterpret_cast<ICoreApplicationView *>(this),
                                    static_cast<IActivatedEventArgs *>(arguments.Get()));
    }

  private:
    ComPtr<Window> window_ = [] {
        ComPtr<Window> window;
        window.Attach(new Window);
        return window;
    }();
    EventSource<ITypedEventHandler<CoreApplicationView *, IActivatedEventArgs *>> activated_;
    bool activated_once_ = false;
};

void step(const char *text) {
    std::fprintf(stderr, "[Runtime] %s\n", text);
    std::fflush(stderr);
}

// ICoreImmersiveApplication as the console has it: MainView only, where the desktop's has
// Views, CreateNewView and then MainView
MIDL_INTERFACE("1ada0e3e-e4a2-4123-b451-dc96bf800419")
IConsoleImmersiveApplication : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MainView(ICoreApplicationView * *value) = 0;
};

class Application : public Class<Application, IActivationFactory, ICoreApplication,
                                 IConsoleImmersiveApplication, ICoreApplicationExit> {
  public:
    static constexpr wchar_t name[] = L"Windows.ApplicationModel.Core.CoreApplication";
    STDMETHODIMP ActivateInstance(IInspectable **instance) override {
        *instance = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP get_Id(HSTRING *value) override { return WindowsCreateString(L"App", 3, value); }
    STDMETHODIMP add_Suspending(IEventHandler<SuspendingEventArgs *> *handler,
                                EventRegistrationToken *token) override {
        return suspending_.Add(handler, token);
    }
    STDMETHODIMP remove_Suspending(EventRegistrationToken token) override {
        return suspending_.Remove(token);
    }
    STDMETHODIMP add_Resuming(IEventHandler<IInspectable *> *handler,
                              EventRegistrationToken *token) override {
        return resuming_.Add(handler, token);
    }
    STDMETHODIMP remove_Resuming(EventRegistrationToken token) override {
        return resuming_.Remove(token);
    }
    STDMETHODIMP get_Properties(Collections::IPropertySet **value) override {
        ComPtr<IInspectable> set;
        const auto result = RoActivateInstance(
            HStringReference(L"Windows.Foundation.Collections.PropertySet").Get(), &set);
        return FAILED(result) ? result : set.CopyTo(value);
    }
    STDMETHODIMP GetCurrentView(ICoreApplicationView **value) override {
        if (!view_)
            return E_FAIL;
        return view_.CopyTo(value);
    }
    // The whole life of the title: returns when its view's Run does.
    STDMETHODIMP Run(IFrameworkViewSource *source) override {
        if (!source)
            return E_POINTER;
        if (view_)
            return E_ILLEGAL_METHOD_CALL;
        view_ = Make<View>();
        ComPtr<IFrameworkView> view;
        auto result = source->CreateView(&view);
        ComPtr<ICoreWindow> window;
        if (SUCCEEDED(result))
            result = view_->get_CoreWindow(&window);
        step("CoreApplication: view created");
        if (SUCCEEDED(result))
            result = view->Initialize(view_.Get());
        if (SUCCEEDED(result))
            result = view->SetWindow(window.Get());
        if (SUCCEEDED(result))
            result = view->Load(HStringReference(L"").Get());
        step("CoreApplication: view loaded");
        if (SUCCEEDED(result))
            result = view_->activate();
        step("CoreApplication: activated, entering the view's Run");
        modules::frame_watchdog(); // a title that never presents a frame counts as stalled from
                                   // here
        if (SUCCEEDED(result))
            result = view->Run();
        step("CoreApplication: the view's Run returned");
        if (SUCCEEDED(result))
            result = view->Uninitialize();
        return result;
    }
    STDMETHODIMP RunWithActivationFactories(IGetActivationFactory *) override { return E_NOTIMPL; }
    // the console's ICoreImmersiveApplication: the one view there is, the main one
    STDMETHODIMP get_MainView(ICoreApplicationView **value) override {
        if (!value)
            return E_POINTER;
        if (!view_) {
            *value = nullptr;
            return E_FAIL;
        }
        return view_.CopyTo(value);
    }
    STDMETHODIMP Exit() override {
        step("CoreApplication: the title asked to exit");
        exiting_.InvokeAll(nullptr, nullptr);
        ExitProcess(0);
    }
    STDMETHODIMP add_Exiting(IEventHandler<IInspectable *> *handler,
                             EventRegistrationToken *token) override {
        return exiting_.Add(handler, token);
    }
    STDMETHODIMP remove_Exiting(EventRegistrationToken token) override {
        return exiting_.Remove(token);
    }

  private:
    ComPtr<View> view_;
    EventSource<IEventHandler<SuspendingEventArgs *>> suspending_;
    EventSource<IEventHandler<IInspectable *>> resuming_;
    EventSource<IEventHandler<IInspectable *>> exiting_;
};
} // namespace

TitleWindow *current_title_window() { return current_window ? &current_window->host() : nullptr; }

IInspectable *application_class(std::wstring_view name) {
    static const auto application = Make<Application>();
    static const auto window = Make<WindowStatics>();
    if (name == Application::name)
        return static_cast<IActivationFactory *>(application.Get());
    if (name == WindowStatics::name)
        return static_cast<IActivationFactory *>(window.Get());
    return nullptr;
}
} // namespace xv::runtime::durango::winrt
