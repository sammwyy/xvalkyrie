#include "runtime/durango/winrt/inert.hpp"
#include <cstdint>
#include <cwchar>

namespace xv::runtime::durango::winrt {
namespace {
struct Row {
    const wchar_t *type;
    GUID iid;
    std::uint8_t method;
    InertMethod behavior;
};
constexpr std::uint8_t digit(char c) {
    return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
}
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", lowercase, as the placeholder log prints it
constexpr GUID guid(const char (&text)[37]) {
    const auto byte = [&](int at) {
        return static_cast<std::uint8_t>(digit(text[at]) << 4 | digit(text[at + 1]));
    };
    const auto word = [&](int at) {
        return static_cast<std::uint16_t>(byte(at) << 8 | byte(at + 2));
    };
    return {static_cast<unsigned long>(word(0)) << 16 | word(4),
            word(9),
            word(14),
            {byte(19), byte(21), byte(24), byte(26), byte(28), byte(30), byte(32), byte(34)}};
}
constexpr auto game_transport_controls = guid("f5ba60d7-9303-44cd-ac0c-4e532702cd00");
constexpr auto current_app = guid("d52dc065-da3f-4685-995e-9b482eb5e603");
constexpr auto user_statics = guid("b4f23a93-7fc6-4a50-b153-950016389e50");
constexpr auto network_statics = guid("5074f851-950d-4165-9c15-365619481eea");
constexpr auto xbox_live_statics = guid("00193376-6a84-4c67-84a5-3b8a8aaea712");

constexpr wchar_t transport[] = L"Windows.Xbox.Media.GameTransportControls";
constexpr wchar_t store[] = L"Windows.ApplicationModel.Store.CurrentApp";
constexpr wchar_t license[] = L"Windows.ApplicationModel.Store.LicenseInformation";
constexpr wchar_t user[] = L"Windows.Xbox.System.User";

constexpr wchar_t network[] = L"Windows.Networking.Connectivity.NetworkInformation";
constexpr wchar_t achievements[] = L"Windows.Xbox.Achievements.AchievementNotifier";
constexpr wchar_t achievement_source[] = L"Windows.Xbox.Achievements.AchievementSource";
constexpr auto achievement_notifier_statics = guid("82ca180b-30e2-4ea0-a62e-b61697a2f334");
constexpr auto achievement_source_interface = guid("3b7e45dc-086e-4535-9a5d-c6e42cdea33c");
constexpr wchar_t chat_session[] = L"Windows.Xbox.Chat.ChatSession";
constexpr wchar_t chat_session_state[] = L"Windows.Xbox.Chat.ChatSessionState";
constexpr auto chat_session2 = guid("8840b65e-eb13-4629-8f63-2854624460ff");
constexpr wchar_t product[] = L"Windows.Xbox.ApplicationModel.Store.Product";
constexpr auto product_statics = guid("76efb705-bdd4-4f6a-8d89-a9511d40afe7");
constexpr wchar_t display_info[] = L"Windows.Xbox.System.UserDisplayInfo";
constexpr wchar_t local_folder[] = L"Windows.Storage.StorageFolder";
constexpr wchar_t web_socket[] = L"Windows.Networking.Sockets.MessageWebSocket";
constexpr wchar_t web_control[] = L"Windows.Networking.Sockets.MessageWebSocketControl";
constexpr wchar_t package_id[] = L"Windows.ApplicationModel.PackageId";
constexpr wchar_t package_folder[] = L"Windows.Storage.StorageFolder.InstalledLocation";
constexpr auto storage_item = guid("4207a996-ca2f-42f7-bde8-8b10457a7f30");
constexpr wchar_t audio_device[] = L"Windows.Xbox.System.AudioDeviceInfo";

constexpr wchar_t system_ui[] = L"Windows.Xbox.UI.SystemUI";
constexpr auto system_ui_statics = guid("1a0dd80e-3f9d-4cfa-8e50-0e2ebd1aa72f");
constexpr auto system_ui_statics7 = guid("01f75706-9df0-44d8-837c-a930da05a74a");

constexpr wchar_t party[] = L"Windows.Xbox.Multiplayer.Party";
constexpr wchar_t party_view[] = L"Windows.Xbox.Multiplayer.PartyView";
constexpr wchar_t party_chat[] = L"Windows.Xbox.Multiplayer.PartyChat";
constexpr wchar_t party_chat_view[] = L"Windows.Xbox.Multiplayer.PartyChatView";
constexpr auto party_statics = guid("e6110c4c-3660-4230-9880-208b149a3ca4");
constexpr auto party_statics2 = guid("87521843-2298-43c4-81f1-c7c50ab794e1");
constexpr auto party_statics3 = guid("15946429-17a1-4fdf-9d67-766ed0f4b868");
constexpr auto party_statics4 = guid("4b907f32-9165-4193-9cce-1d5ccddd060e");
constexpr auto party_chat_statics = guid("cc1bcf39-99f3-42a2-afb6-cdc07b29755f");

constexpr wchar_t transfer_manager[] = L"Windows.Xbox.Management.Deployment.PackageTransferManager";
constexpr wchar_t kinect[] = L"Windows.Kinect.KinectSensor";
constexpr wchar_t speech_recognizer[] = L"Windows.Xbox.Speech.Recognition.SpeechRecognizer";
constexpr wchar_t socket_description[] = L"Windows.Xbox.Networking.SecureDeviceSocketDescription";
constexpr wchar_t association_template[] =
    L"Windows.Xbox.Networking.SecureDeviceAssociationTemplate";
constexpr wchar_t association[] = L"Windows.Xbox.Networking.SecureDeviceAssociation";
constexpr wchar_t device_address[] = L"Windows.Xbox.Networking.SecureDeviceAddress";
constexpr wchar_t quality_of_service[] = L"Windows.Xbox.Networking.QualityOfService";
constexpr wchar_t network_cache[] = L"Windows.Xbox.Networking.NetworkCache";
constexpr wchar_t error_messages[] = L"Windows.Xbox.Networking.ErrorMessages";

constexpr wchar_t clip_capture[] = L"Windows.Xbox.Media.Capture.ApplicationClipCapture";
constexpr wchar_t clip_content[] = L"Windows.Xbox.Media.Capture.ClipContentAttributes";
constexpr wchar_t clip_info[] = L"Windows.Xbox.Media.Capture.ApplicationClipInfo";
constexpr wchar_t clip[] = L"Windows.Xbox.Media.Capture.ApplicationClip";
constexpr wchar_t clip_query[] = L"Windows.Xbox.Media.Capture.ApplicationClipQuery";
constexpr wchar_t clip_event[] = L"Windows.Xbox.Media.Capture.ApplicationClipEventData";
constexpr wchar_t clip_metadata[] = L"Windows.Xbox.Media.Capture.ApplicationClipUpdateMetadataInfo";
constexpr wchar_t diagnostic_capture[] = L"Windows.Xbox.Media.Capture.DiagnosticCapture";
// ApplicationClipError::ServiceNotRunning: game DVR is not there
constexpr long no_recording = -2110640127;
// HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE)
constexpr long network_unreachable = static_cast<long>(0x800704CF);

constexpr Row rows[] = {
    {transport, game_transport_controls, 14, {Inert::accept}}, // (bool)
    {transport, game_transport_controls, 16, {Inert::accept}}, // (bool), one per button
    {transport, game_transport_controls, 18, {Inert::accept}},
    {transport, game_transport_controls, 20, {Inert::accept}},
    {transport, game_transport_controls, 22, {Inert::accept}},
    {transport, game_transport_controls, 24, {Inert::accept}},
    {transport, game_transport_controls, 25, {Inert::event_add}}, // button pressed
    {transport, game_transport_controls, 27, {Inert::event_add}}, // property changed
    {transport, game_transport_controls, 28, {Inert::accept}},

    // public interfaces ICurrentApp and ILicenseInformation: a full license that never changes
    {store, current_app, 6, {Inert::object, license}},
    {license, direct, 7, {Inert::true8}},      // IsActive
    {license, direct, 8, {Inert::zero8}},      // IsTrial
    {license, direct, 9, {Inert::never}},      // ExpirationDate
    {license, direct, 10, {Inert::event_add}}, // LicenseChanged
    // the console's own interface of it: the user the license is for
    {license,
     guid("aa98c583-a3c8-4f51-a5bc-463ae0c23eaa"),
     6,
     {Inert::console_string, L"xbox_user_id"}},

    // one user, signed in from the start and for good
    {user, user_statics, 6, {Inert::shared_list, user}}, // Users
    {user, user_statics, 7, {Inert::event_add}},
    {user, user_statics, 10, {Inert::event_add}},
    {user, user_statics, 12, {Inert::event_add}},
    {user, user_statics, 26, {Inert::event_add}},

    // public interface INetworkInformationStatics
    {network, network_statics, 6, {Inert::empty_list}}, // GetConnectionProfiles: none
    {network, network_statics, 7, {Inert::zero64}},     // GetInternetConnectionProfile: none
    {network, network_statics, 12, {Inert::event_add}}, // NetworkStatusChanged

    // IAchievementNotifierStatics: GetTitleIdFilteredSource, GetUnfilteredSource. The
    // AchievementUnlocked event of IAchievementSource is never raised: nothing unlocks here.
    {achievements, achievement_notifier_statics, 6, {Inert::object, achievement_source}},
    {achievements, achievement_notifier_statics, 7, {Inert::object, achievement_source}},
    {achievement_source, direct, 6, {Inert::event_add}},
    {achievement_source, direct, 7, {Inert::accept}},
    {achievement_source, achievement_source_interface, 6, {Inert::event_add}},
    {achievement_source, achievement_source_interface, 7, {Inert::accept}},

    // Chat with no microphone and no headset anywhere: sessions hold channels, their state
    // has no capture source and no render target, and nothing changes.
    // IChatSessionFactory: (TimeSpan, features, out) and (TimeSpan, affinity, features, out)
    {chat_session,
     guid("53cb6065-c211-4b21-a3ef-cd544d22aba1"),
     6,
     {Inert::object_at, chat_session, 3}},
    {chat_session,
     guid("53cb6065-c211-4b21-a3ef-cd544d22aba1"),
     7,
     {Inert::object_at, chat_session, 4}},
    {chat_session, direct, 6, {Inert::vector}},    // Channels
    {chat_session, direct, 7, {Inert::event_add}}, // StateChangedEvent
    {chat_session, direct, 8, {Inert::accept}},
    {chat_session, direct, 9, {Inert::operation, chat_session_state}}, // GetStateAsync
    {chat_session_state, direct, 6, {Inert::empty_list}},              // CaptureSources
    {chat_session_state, direct, 7, {Inert::empty_list}},              // RenderTargets
    // IChatSession2: TranscriptionEnabled, TranscribedTextReadyEvent
    {chat_session, chat_session2, 6, {Inert::zero32}},
    {chat_session, chat_session2, 7, {Inert::accept}},
    {chat_session, chat_session2, 9, {Inert::event_add}},
    {chat_session, chat_session2, 10, {Inert::accept}},

    // IProductStatics: the store dialogs close at once, every privilege is there, and
    // nothing is ever purchased
    {product, product_statics, 6, {Inert::action_at, nullptr, 3}},          // ShowPurchaseAsync
    {product, product_statics, 7, {Inert::action_at, nullptr, 3}},          // ShowDetailsAsync
    {product, product_statics, 8, {Inert::action_at, nullptr, 5}},          // ShowMarketplaceAsync
    {product, product_statics, 9, {Inert::action_at, nullptr, 3}},          // ShowRedeemCodeAsync
    {product, product_statics, 10, {Inert::action_at, nullptr, 4}},         // ShowSubscriptionAsync
    {product, product_statics, 11, {Inert::zero_operation_at, nullptr, 5}}, // CheckPrivilegeAsync
    {product, product_statics, 12, {Inert::zero_operation_at, nullptr, 5}}, // CheckPrivilegesAsync
    {product, product_statics, 13, {Inert::event_add}},                     // ProductPurchased
    {product, product_statics, 14, {Inert::accept}},

    // event registrations for things that never happen here
    {L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager",
     guid("971feaaf-98f8-4038-b8c1-3a5f1a043b97"),
     7,
     {Inert::event_add}}, // package installed
    // public ICoreAcceleratorKeys: AcceleratorKeyActivated
    {L"Windows.UI.Core.CoreDispatcher",
     guid("9ffdf7f5-b8c9-4ef0-b7d2-1de626561fc8"),
     6,
     {Inert::event_add}},
    {L"Windows.UI.Core.CoreDispatcher",
     guid("9ffdf7f5-b8c9-4ef0-b7d2-1de626561fc8"),
     7,
     {Inert::accept}},
    // public IMouseDeviceStatics::GetForCurrentView and IMouseDevice::MouseMoved
    {L"Windows.Devices.Input.MouseDevice",
     guid("f5a82ce3-4a2d-4004-b386-c74dc69be979"),
     6,
     {Inert::shared, L"Windows.Devices.Input.MouseDevice"}},
    {L"Windows.Devices.Input.MouseDevice", direct, 6, {Inert::event_add}},
    {L"Windows.Devices.Input.MouseDevice", direct, 7, {Inert::accept}},
    // the console's ICoreApplicationResourceAvailability: all of the console is the title's
    {L"Windows.ApplicationModel.Core.CoreApplication",
     guid("9ff3799e-6e83-468c-a5b0-501ce6bb0fa1"),
     6,
     {Inert::zero32}}, // ResourceAvailability_Full
    {L"Windows.ApplicationModel.Core.CoreApplication",
     guid("9ff3799e-6e83-468c-a5b0-501ce6bb0fa1"),
     7,
     {Inert::event_add}},
    {L"Windows.ApplicationModel.Core.CoreApplication",
     guid("9ff3799e-6e83-468c-a5b0-501ce6bb0fa1"),
     8,
     {Inert::accept}},
    {L"Windows.Xbox.Multiplayer.PartyConfig",
     guid("2fcec780-2e70-437a-ae46-72298ae2bac2"),
     7,
     {Inert::accept}}, // (bool)
    {L"Windows.Xbox.Multiplayer.PartyConfig",
     guid("6938fc66-ed0c-4b86-a240-009da675a0ec"),
     7,
     {Inert::accept}}, // (32-bit value)
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     11,
     {Inert::event_add}},
    {L"Windows.Xbox.Input.Controller", // Controllers: the gamepad is the one there is
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     6,
     {Inert::controllers}},
    {L"Windows.Xbox.Input.Controller", // GetControllerById(id, IController **)
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     15,
     {Inert::controller_at, nullptr, 2}},
    // GetControllersOrderedLeftToRight(filter, IVectorView<IController> **)
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     17,
     {Inert::controllers, nullptr, 2}},
    {L"Windows.Xbox.Input.Controller", // ControllerAdded, ControllerRemoved
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     7,
     {Inert::event_add}},
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     9,
     {Inert::event_add}},
    {L"Windows.Xbox.System.User",
     guid("b4f23a93-7fc6-4a50-b153-950016389e50"),
     9,
     {Inert::shared_list, L"Windows.Xbox.System.User"}},
    {L"Windows.Xbox.System.User",
     guid("b4f23a93-7fc6-4a50-b153-950016389e50"),
     18,
     {Inert::event_add}},
    {L"Windows.Xbox.System.User",
     guid("b4f23a93-7fc6-4a50-b153-950016389e50"),
     20,
     {Inert::event_add}},
    {L"Windows.Xbox.System.User",
     guid("b4f23a93-7fc6-4a50-b153-950016389e50"),
     22,
     {Inert::event_add}},
    {L"Windows.Xbox.Input.Gamepad",
     guid("9776ff56-9bfd-4581-ad45-b645bba526d6"),
     6,
     {Inert::one64}},
    {L"Windows.Xbox.Input.Gamepad",
     guid("9776ff56-9bfd-4581-ad45-b645bba526d6"),
     8,
     {Inert::shared, L"Windows.Xbox.System.User"}},
    {L"Windows.Xbox.System.User", direct, 11, {Inert::true8}},
    {L"Windows.Xbox.System.User", direct, 10, {Inert::zero8}},
    {L"Windows.Xbox.System.User", direct, 15, {Inert::console_string, L"xbox_user_id"}},
    {L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager",
     guid("971feaaf-98f8-4038-b8c1-3a5f1a043b97"),
     6,
     {Inert::empty_list2}},
    // the display info of the one user, the same object every time it is asked for
    {L"Windows.Xbox.System.User",
     direct,
     9,
     {Inert::shared, L"Windows.Xbox.System.UserDisplayInfo"}},
    // IUserDisplayInfo: Gamertag, GamerScore, ApplicationDisplayName, GameDisplayName,
    // Reputation, AgeGroup, all from Options > Profiles
    {display_info, direct, 6, {Inert::console_string, L"gamertag"}},
    {display_info, direct, 7, {Inert::console32, L"gamerscore"}},
    {display_info, direct, 8, {Inert::console_string, L"gamertag"}},
    {display_info, direct, 9, {Inert::console_string, L"gamertag"}},
    {display_info, direct, 10, {Inert::zero32}},
    {display_info, direct, 11, {Inert::console32, L"age_group"}},
    // IKinectSensorStatics::GetDefault: a sensor that is never available, as with none plugged
    // in. IKinectSensor::Open succeeds all the same.
    {kinect, guid("c421d46b-5084-45d6-bdf0-57151f81d0d9"), 6, {Inert::shared, kinect}},
    // a console interface of CoreApplication with one flag: its getter and its setter
    {L"Windows.ApplicationModel.Core.CoreApplication",
     guid("42a6a529-3cc6-4867-b5b7-6d1faf5325f9"),
     6,
     {Inert::zero8}},
    {L"Windows.ApplicationModel.Core.CoreApplication",
     guid("42a6a529-3cc6-4867-b5b7-6d1faf5325f9"),
     7,
     {Inert::accept}},
    {kinect, direct, 8, {Inert::accept}},
    {kinect, direct, 10, {Inert::true8}}, // IsOpen
    {kinect, direct, 11, {Inert::zero8}}, // IsAvailable
    {kinect, direct, 14, {Inert::object, L"Windows.Kinect.BodyFrameSource"}},
    {L"Windows.Kinect.BodyFrameSource", direct, 9, {Inert::number32, nullptr, 6}}, // BodyCount
    {L"Windows.Kinect.BodyFrameSource",
     direct,
     10,
     {Inert::object, L"Windows.Kinect.BodyFrameReader"}},
    {L"Windows.Kinect.BodyFrameReader", direct, 8, {Inert::zero64}}, // no frame
    {L"Windows.Kinect.BodyFrameReader", direct, 6, {Inert::event_add}},
    {L"Windows.Xbox.Input.BodyController",
     guid("99ff8b9b-c70e-496a-a23b-ec4b7996b8ff"),
     7,
     {Inert::event_add}},
    {L"Windows.Xbox.Input.BodyController",
     guid("99ff8b9b-c70e-496a-a23b-ec4b7996b8ff"),
     9,
     {Inert::event_add}},
    // SmartGlass: no companion devices, and none to come
    {L"Windows.Xbox.SmartGlass.SmartGlassDeviceWatcher",
     guid("0e9b9ff2-8b3e-4e4e-ad80-6df4ee94737b"),
     6,
     {Inert::event_add}},
    {L"Windows.Xbox.SmartGlass.SmartGlassDeviceWatcher",
     guid("0e9b9ff2-8b3e-4e4e-ad80-6df4ee94737b"),
     8,
     {Inert::event_add}},
    {L"Windows.Xbox.SmartGlass.SmartGlassDevice",
     guid("913ef096-097b-4672-be90-f3a75ab5ca11"),
     7,
     {Inert::list_operation}},
    {L"Windows.Xbox.System.User", direct, 7, {Inert::audio_devices}},
    // IAudioDeviceInfo of the headset: the host's communications output, shared, with no
    // microphone to unmute
    {audio_device, direct, 6, {Inert::console_string, L"audio_device_id"}},
    {audio_device, direct, 7, {Inert::number32, nullptr, 1}}, // DeviceType: render
    {audio_device, direct, 8, {Inert::number32, nullptr, 2}}, // Sharing: shared
    {audio_device, direct, 9, {Inert::zero32}},               // Category: communications
    {audio_device, direct, 10, {Inert::true8}},               // IsMicrophoneMuted
    // ShowAccountPickerAsync(controller, options, IAsyncOperation<AccountPickerResult> **)
    {L"Windows.Xbox.UI.SystemUI",
     guid("1a0dd80e-3f9d-4cfa-8e50-0e2ebd1aa72f"),
     10,
     {Inert::operation_at, L"Windows.Xbox.UI.AccountPickerResult", 3}},
    {L"Windows.Xbox.UI.AccountPickerResult", direct, 6, {Inert::shared, user}}, // User
    {user, direct, 6, {Inert::one32}},                                          // Id
    {user, direct, 8, {Inert::controllers}},                                    // Controllers
    {user, direct, 12, {Inert::zero32}},                                        // Location
    {user, direct, 13, {Inert::zero64}},                                        // Sponsor: none
    {user, direct, 14, {Inert::string, L"0000000000000001"}},                   // XboxUserHash
    // GetTokenAndSignatureAsync, three overloads: there is no network to sign with
    {user, direct, 16, {Inert::offline, nullptr, 4}},
    {user, direct, 17, {Inert::offline, nullptr, 6}},
    {user, direct, 18, {Inert::offline, nullptr, 5}},
    // IPackageStatics::Current: the one package there is
    {L"Windows.ApplicationModel.Package",
     guid("4e534bdf-2960-4878-97a4-9624deb72f2d"),
     6,
     {Inert::shared, L"Windows.ApplicationModel.Package"}},
    // IPackage::Id and the public IPackageId, from the manifest's identity
    {L"Windows.ApplicationModel.Package", direct, 6, {Inert::shared, package_id}},
    {package_id, direct, 6, {Inert::title_string, L"package_name"}},
    {package_id, direct, 7, {Inert::title_version}},
    {package_id, direct, 8, {Inert::number32, nullptr, 9}}, // ProcessorArchitecture: x64
    {package_id, direct, 9, {Inert::zero64}},               // ResourceId: none
    {package_id, direct, 10, {Inert::title_string, L"package_publisher"}},
    {package_id, direct, 11, {Inert::title_string, L"package_publisher_id"}},
    {package_id, direct, 12, {Inert::title_string, L"package_full_name"}},
    {package_id, direct, 13, {Inert::title_string, L"package_family_name"}},
    // IPackage::InstalledLocation: the folder the title was unpacked in
    {L"Windows.ApplicationModel.Package", direct, 7, {Inert::shared, package_folder}},
    {package_folder, storage_item, 6, {Inert::console_string, L"package_folder_name"}},
    {package_folder, storage_item, 7, {Inert::console_string, L"package_folder"}},
    // PackageTransferWatcher: the whole package is on disk, nothing is being transferred
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("a306e506-12f9-460a-b0ae-50ba328c786c"),
     6,
     {Inert::lookup2, L"Windows.Xbox.Management.Deployment.PackageTransferWatcher"}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("a306e506-12f9-460a-b0ae-50ba328c786c"),
     7,
     {Inert::lookup, L"Windows.Xbox.Management.Deployment.PackageTransferWatcher"}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 6, {Inert::empty_list}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 7, {Inert::event_add}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 8, {Inert::accept}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 9, {Inert::event_add}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 10, {Inert::accept}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 11, {Inert::event_add}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 12, {Inert::accept}},
    // IPackageTransferWatcherTransferInfo: TransferStatus, TransferType, the status event
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("ba1bad13-11e1-43ce-be44-845759796a7f"),
     6,
     {Inert::zero32}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("ba1bad13-11e1-43ce-be44-845759796a7f"),
     7,
     {Inert::zero32}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("ba1bad13-11e1-43ce-be44-845759796a7f"),
     8,
     {Inert::event_add}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("ba1bad13-11e1-43ce-be44-845759796a7f"),
     9,
     {Inert::accept}},
    // IXboxLiveConfigurationStatics: TitleId (decimal), PrimaryServiceConfigId, SandboxId
    {L"Windows.Xbox.Services.XboxLiveConfiguration",
     xbox_live_statics,
     6,
     {Inert::title_string, L"title_id"}},
    {L"Windows.Xbox.Services.XboxLiveConfiguration",
     xbox_live_statics,
     7,
     {Inert::title_string, L"service_config_id"}},
    {L"Windows.Xbox.Services.XboxLiveConfiguration",
     xbox_live_statics,
     8,
     {Inert::title_string, L"sandbox_id"}},

    // ISystemUIStatics: the guide, profile cards, invites, party and achievements are not
    // there and close at once; the keyboard is a window of the host (system_ui.cpp)
    {system_ui, system_ui_statics, 6, {Inert::action_at, nullptr, 3}},  // ShowEpgAsync
    {system_ui, system_ui_statics, 7, {Inert::action_at, nullptr, 2}},  // ShowMiniguideAsync
    {system_ui, system_ui_statics, 8, {Inert::action_at, nullptr, 3}},  // ShowProfileCardAsync
    {system_ui, system_ui_statics, 9, {Inert::action_at, nullptr, 2}},  // ShowSendInvitesAsync
    {system_ui, system_ui_statics, 11, {Inert::keyboard}},              // ShowVirtualKeyboardAsync
    {system_ui, system_ui_statics, 12, {Inert::action_at, nullptr, 2}}, // LaunchPartyAsync
    {system_ui, system_ui_statics, 13, {Inert::action_at, nullptr, 3}}, // LaunchAchievementsAsync
    {system_ui, system_ui_statics7, 6, {Inert::keyboard_options}},
    {system_ui, system_ui_statics7, 7, {Inert::action_at, nullptr, 4}}, // ShowSendGameInvites
    {system_ui, system_ui_statics7, 8, {Inert::action_at, nullptr, 5}},
    {system_ui, guid("197625d0-af45-4f9c-b068-d5bda0e60958"), 6, {Inert::accept}},

    // IPartyStatics..4: no party. Its view is empty, registering sessions succeeds and the
    // events are never raised.
    {party, party_statics, 6, {Inert::action_at, nullptr, 3}},          // RegisterGameSessionAsync
    {party, party_statics, 7, {Inert::action_at, nullptr, 3}},          // RegisterMatchSessionAsync
    {party, party_statics, 8, {Inert::action_at, nullptr, 3}},          // InviteToPartyAsync
    {party, party_statics, 9, {Inert::operation, party_view}},          // GetPartyViewAsync
    {party, party_statics, 10, {Inert::action_at, nullptr, 3}},         // AddLocalUsersAsync
    {party, party_statics, 11, {Inert::action_at, nullptr, 3}},         // PullInactivePlayersAsync
    {party, party_statics, 12, {Inert::action_at, nullptr, 3}},         // PullReservedPlayersAsync
    {party, party_statics, 13, {Inert::list_operation_at, nullptr, 2}}, // GetAvailableGamePlayers
    {party, party_statics, 14, {Inert::list_operation_at, nullptr, 2}}, // ...GroupedByDevice
    {party, party_statics, 15, {Inert::action_at, nullptr, 2}},         // SwitchPartyTitleAsync
    {party, party_statics, 16, {Inert::event_add}},                     // GameSessionReady
    {party, party_statics, 17, {Inert::accept}},
    {party, party_statics, 18, {Inert::event_add}}, // PartyStateChanged
    {party, party_statics, 19, {Inert::accept}},
    {party, party_statics, 20, {Inert::event_add}}, // PartyRosterChanged
    {party, party_statics, 21, {Inert::accept}},
    {party, party_statics, 22, {Inert::event_add}}, // MatchStatusChanged
    {party, party_statics, 23, {Inert::accept}},
    {party, party_statics, 24, {Inert::event_add}}, // GamePlayersChanged
    {party, party_statics, 25, {Inert::accept}},
    {party, party_statics2, 6, {Inert::operation_at, party_view, 2}}, // GetPartyViewAsyncEx
    {party, party_statics3, 6, {Inert::action_at, nullptr, 2}},       // RemoveLocalUsersAsync
    {party, party_statics3, 7, {Inert::zero64}},                      // PartyId: none
    {party, party_statics3, 8, {Inert::action_at, nullptr, 3}}, // DisassociateGameSessionAsync
    {party, party_statics4, 6, {Inert::action_at, nullptr, 4}}, // RegisterGameSessionCompare
    {party, party_statics4, 7, {Inert::action_at, nullptr, 4}}, // RegisterMatchSessionCompare
    {party, party_statics4, 8, {Inert::list_operation_at, nullptr, 3}}, // GetUserPartyAssociations
    {party, party_statics4, 9, {Inert::operation_at, party_view, 3}},   // GetPartyViewByPartyId
    {party, party_statics4, 10, {Inert::action_at, nullptr, 3}},        // JoinPartyByIdAsync
    {party, party_statics4, 11, {Inert::zero32}},                       // Joinability
    {party, party_statics4, 12, {Inert::accept}},
    // IPartyView: no members, no sessions, not in another title
    {party_view, direct, 6, {Inert::empty_list}},
    {party_view, direct, 7, {Inert::empty_list}},
    {party_view, direct, 8, {Inert::empty_list}},
    {party_view, direct, 9, {Inert::zero64}},
    {party_view, direct, 10, {Inert::zero64}},
    {party_view, direct, 11, {Inert::zero8}},
    // IPartyChatStatics: party chat is off and stays so
    {party_chat, party_chat_statics, 6, {Inert::operation, party_chat_view}},
    {party_chat, party_chat_statics, 7, {Inert::event_add}},
    {party_chat, party_chat_statics, 8, {Inert::accept}},
    {party_chat, party_chat_statics, 9, {Inert::zero8}},  // IsPartyChatActive
    {party_chat, party_chat_statics, 10, {Inert::zero8}}, // IsPartyChatSuppressed
    {party_chat, party_chat_statics, 11, {Inert::accept}},
    {party_chat, party_chat_statics, 12, {Inert::event_add}},
    {party_chat, party_chat_statics, 13, {Inert::accept}},
    {party_chat, party_chat_statics, 14, {Inert::event_add}},
    {party_chat, party_chat_statics, 15, {Inert::accept}},
    {party_chat_view, direct, 6, {Inert::empty_list}}, // Members
    // PartyConfig getters, next to the setters above
    {L"Windows.Xbox.Multiplayer.PartyConfig",
     guid("2fcec780-2e70-437a-ae46-72298ae2bac2"),
     6,
     {Inert::zero8}},
    {L"Windows.Xbox.Multiplayer.PartyConfig",
     guid("6938fc66-ed0c-4b86-a240-009da675a0ec"),
     6,
     {Inert::zero32}},

    // Secure device associations without a network: templates exist, creating an
    // association fails as unreachable, there is no association to look up, and the local
    // address is one object with strict NAT
    {association_template, direct, 6, {Inert::event_add}}, // AssociationIncoming
    {association_template, direct, 7, {Inert::accept}},
    {association_template, direct, 8, {Inert::offline, nullptr, 3}},  // CreateAssociationAsync
    {association_template, direct, 9, {Inert::offline, nullptr, 5}},  // ...ForPortsAsync
    {association_template, direct, 10, {Inert::offline, nullptr, 2}}, // CreateCertificateRequest
    {association_template, direct, 11, {Inert::failed_action_at, nullptr, 2, network_unreachable}},
    // Name, the two socket descriptions, AllowedUsages, RelyingParty, the parser and quality
    // of service lists, MultiplayerSessionRequirement: a template that describes nothing
    {association_template, direct, 12, {Inert::zero64}},
    {association_template, direct, 13, {Inert::object, socket_description}},
    {association_template, direct, 14, {Inert::object, socket_description}},
    {association_template, direct, 15, {Inert::zero32}},
    {association_template, direct, 16, {Inert::zero64}},
    {association_template, direct, 17, {Inert::empty_list}},
    {association_template, direct, 18, {Inert::empty_list}},
    {association_template, direct, 19, {Inert::zero32}},
    {association_template, direct, 20, {Inert::empty_list}}, // Associations
    // SecureDeviceSocketDescription: Name, IpProtocol, the bound port range (two 16-bit
    // values, 0 for any port), AllowedUsages
    {socket_description, direct, 6, {Inert::zero64}},
    {socket_description, direct, 7, {Inert::zero32}},
    {socket_description, direct, 8, {Inert::zero32}},
    {socket_description, direct, 9, {Inert::zero32}},
    {socket_description, direct, 10, {Inert::zero32}},
    {association,
     guid("15661441-f13a-46e4-8ac2-f474d59b1f0c"),
     6,
     {Inert::null_at, nullptr, 5}}, // GetAssociationBySocketAddressBytes
    {association,
     guid("15661441-f13a-46e4-8ac2-f474d59b1f0c"),
     7,
     {Inert::null_at, nullptr, 5}}, // GetAssociationByHostNamesAndPorts
    {device_address,
     guid("02addb0f-4880-4b1e-98b8-ee638ad89b55"),
     9,
     {Inert::shared, device_address}},               // GetLocal
    {device_address, direct, 6, {Inert::event_add}}, // BufferChanged
    {device_address, direct, 7, {Inert::accept}},
    {device_address, direct, 10, {Inert::zero64}},               // GetBase64String: empty
    {device_address, direct, 11, {Inert::true8}},                // IsLocal
    {device_address, direct, 12, {Inert::number32, nullptr, 2}}, // NetworkAccessType: strict
    {quality_of_service,
     guid("d7d51fb8-4e8e-4e11-b2a4-930f9c344a6a"),
     6,
     {Inert::offline, nullptr, 5}}, // MeasureQualityOfServiceAsync
    {quality_of_service, guid("d7d51fb8-4e8e-4e11-b2a4-930f9c344a6a"), 7, {Inert::accept}},
    {quality_of_service, guid("d7d51fb8-4e8e-4e11-b2a4-930f9c344a6a"), 8, {Inert::accept}},
    {quality_of_service, guid("d7d51fb8-4e8e-4e11-b2a4-930f9c344a6a"), 9, {Inert::accept}},
    {network_cache, guid("bfff4b75-05a4-49b0-9383-c29df62a8da2"), 6, {Inert::accept}},
    {network_cache, guid("bfff4b75-05a4-49b0-9383-c29df62a8da2"), 7, {Inert::accept}},
    // GetMessageForError(code, HSTRING *): no message, the title shows its own
    {error_messages, guid("0759380d-a6d3-443a-9441-e45edc7677e6"), 6, {Inert::null_at, nullptr, 2}},

    // Game DVR is not running: recording fails as the console says when it is off, there
    // are no clips, and the metadata a title sets is accepted and forgotten
    {clip_capture, guid("82845b56-3150-4058-947a-b567537de0c2"), 6, {Inert::action}},
    {clip_capture, guid("82845b56-3150-4058-947a-b567537de0c2"), 7, {Inert::action}},
    {clip_capture, guid("2ac83090-107a-4cd7-8376-5188d3fcfa4e"), 6, {Inert::shared, clip_content}},
    {clip_capture, guid("2ac83090-107a-4cd7-8376-5188d3fcfa4e"), 7, {Inert::accept}},
    {clip_content, direct, 6, {Inert::zero32}},
    {clip_content, direct, 7, {Inert::accept}},
    {clip_capture, direct, 6, {Inert::failed_action_at, nullptr, 2, no_recording}},
    {clip_capture, direct, 7, {Inert::failed_progress_at, nullptr, 2, no_recording}},
    {clip_capture, direct, 8, {Inert::failed_progress_at, nullptr, 5, no_recording}},
    {clip_capture, direct, 9, {Inert::accept}},     // CancelRecord
    {clip_capture, direct, 10, {Inert::event_add}}, // Failed
    {clip_capture, direct, 11, {Inert::accept}},
    {clip_capture, direct, 12, {Inert::event_add}}, // RecordLimitationExceeded
    {clip_capture, direct, 13, {Inert::accept}},
    {clip_info, guid("509ccd19-fee4-45ab-8daa-7895328d452f"), 6, {Inert::lookup, clip_info}},
    {clip_info, direct, 6, {Inert::zero64}},
    {clip_info, direct, 7, {Inert::accept}},
    {clip_info, direct, 8, {Inert::empty_list}},
    {clip_info, direct, 9, {Inert::accept}},
    {clip_info, direct, 10, {Inert::zero64}},
    {clip_info, direct, 11, {Inert::accept}},
    {clip_info, direct, 12, {Inert::zero64}},
    {clip_info, direct, 13, {Inert::accept}},
    {clip_info, direct, 14, {Inert::zero64}},
    {clip_info, direct, 15, {Inert::accept}},
    {clip_info, direct, 16, {Inert::zero64}},
    {clip_info, direct, 17, {Inert::accept}},
    {clip_info, direct, 18, {Inert::zero64}},
    {clip_info, direct, 19, {Inert::accept}},
    {clip,
     guid("c0c846ba-0d69-4b4b-ac8b-dba211322f1d"),
     6,
     {Inert::failed_action_at, nullptr, 6, no_recording}}, // UpdateClipMetadataAsync
    {clip,
     guid("c0c846ba-0d69-4b4b-ac8b-dba211322f1d"),
     7,
     {Inert::failed_action_at, nullptr, 5, no_recording}},           // DeleteClipAsync
    {clip_query, direct, 6, {Inert::list_operation_at, nullptr, 2}}, // GetClipsAsync
    {clip_query, direct, 7, {Inert::list_operation_at, nullptr, 3}},
    {clip_query, direct, 8, {Inert::list_operation_at, nullptr, 1}}, // GetNextClipsAsync
    {clip_query,
     guid("c2d95416-127e-4342-b433-d95e7dc31baa"),
     6,
     {Inert::list_operation_at, nullptr, 3}},
    {clip_metadata, direct, 6, {Inert::zero64}},
    {clip_metadata, direct, 7, {Inert::accept}},
    {clip_metadata, direct, 8, {Inert::zero64}},
    {clip_metadata, direct, 9, {Inert::accept}},
    {clip_metadata, direct, 10, {Inert::zero32}},
    // IApplicationClipEventData: Int32 SectionId, String PlayerSessionId, then four Int32
    // and three Single, each a getter and a setter
    {clip_event, direct, 6, {Inert::zero32}},
    {clip_event, direct, 7, {Inert::accept}},
    {clip_event, direct, 8, {Inert::zero64}},
    {clip_event, direct, 9, {Inert::accept}},
    {clip_event, direct, 10, {Inert::zero32}},
    {clip_event, direct, 11, {Inert::accept}},
    {clip_event, direct, 12, {Inert::zero32}},
    {clip_event, direct, 13, {Inert::accept}},
    {clip_event, direct, 14, {Inert::zero32}},
    {clip_event, direct, 15, {Inert::accept}},
    {clip_event, direct, 16, {Inert::zero32}},
    {clip_event, direct, 17, {Inert::accept}},
    {clip_event, direct, 18, {Inert::zero32}},
    {clip_event, direct, 19, {Inert::accept}},
    {clip_event, direct, 20, {Inert::zero32}},
    {clip_event, direct, 21, {Inert::accept}},
    {clip_event, direct, 22, {Inert::zero32}},
    {clip_event, direct, 23, {Inert::accept}},
    {diagnostic_capture,
     guid("39f20721-379c-4255-b558-c133e3359857"),
     6,
     {Inert::failed_action_at, nullptr, 3, no_recording}}, // RecordTemporaryClipAsync

    // IInstalledSpeechRecognizersStatic: no microphone, so no recognizer
    {L"Windows.Xbox.Speech.Recognition.InstalledSpeechRecognizers",
     guid("606a630f-c5c3-4dfa-9950-d98bfe772c8f"),
     6,
     {Inert::empty_list}},
    {L"Windows.Xbox.Speech.Recognition.InstalledSpeechRecognizers",
     guid("606a630f-c5c3-4dfa-9950-d98bfe772c8f"),
     7,
     {Inert::zero64}},

    // IUserStatics events: sign-in and sign-out, display info changes;
    // removes of all of them
    {user, user_statics, 24, {Inert::event_add}}, // SignInCompleted
    {user, user_statics, 28, {Inert::event_add}}, // SignOutCompleted
    {user, user_statics, 30, {Inert::event_add}}, // UserDisplayInfoChanged
    {user, user_statics, 8, {Inert::accept}},
    {user, user_statics, 11, {Inert::accept}},
    {user, user_statics, 13, {Inert::accept}},
    {user, user_statics, 19, {Inert::accept}},
    {user, user_statics, 21, {Inert::accept}},
    {user, user_statics, 23, {Inert::accept}},
    {user, user_statics, 25, {Inert::accept}},
    {user, user_statics, 27, {Inert::accept}},
    {user, user_statics, 29, {Inert::accept}},
    {user, user_statics, 31, {Inert::accept}},
    {display_info, direct, 12, {Inert::empty_list}}, // Privileges
    // ICoreApplicationContext: CurrentUser is the one user;
    // ICoreApplicationContext2/3: setting it is accepted, CurrentSystemUser is the same
    {L"Windows.Xbox.ApplicationModel.Core.CoreApplicationContext",
     guid("d8d4c59f-b9ac-4d41-842d-a01c59b617e7"),
     6,
     {Inert::shared, user}},
    {L"Windows.Xbox.ApplicationModel.Core.CoreApplicationContext",
     guid("d8d4c59f-b9ac-4d41-842d-a01c59b617e7"),
     7,
     {Inert::event_add}},
    {L"Windows.Xbox.ApplicationModel.Core.CoreApplicationContext",
     guid("e555452e-8e16-46a6-bf31-34189b6cad40"),
     6,
     {Inert::accept}},
    {L"Windows.Xbox.ApplicationModel.Core.CoreApplicationContext",
     guid("52db8037-0ecb-48f6-974d-5796ecf34b69"),
     6,
     {Inert::shared, user}},
    {L"Windows.Xbox.ApplicationModel.Core.CoreApplicationContext",
     guid("52db8037-0ecb-48f6-974d-5796ecf34b69"),
     7,
     {Inert::event_add}},
    {L"Windows.Xbox.ApplicationModel.Core.CoreApplicationContext",
     guid("52db8037-0ecb-48f6-974d-5796ecf34b69"),
     8,
     {Inert::accept}},
    {L"Windows.Xbox.ApplicationModel.Core.CoreApplicationContext",
     guid("d8d4c59f-b9ac-4d41-842d-a01c59b617e7"),
     8,
     {Inert::accept}},
    {L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager",
     guid("971feaaf-98f8-4038-b8c1-3a5f1a043b97"),
     9,
     {Inert::event_add}},
    {L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager",
     guid("971feaaf-98f8-4038-b8c1-3a5f1a043b97"),
     10,
     {Inert::accept}},

    // IApplicationDataStatics::Current and the console's IApplicationData and IStorageItem,
    // which are not the desktop's: a title's call sites read LocalFolder at 6 and, through
    // IStorageItem, Name at 6 and Path at 7. The folder is in the title's save folder.
    {L"Windows.Storage.ApplicationData",
     guid("5612147b-e843-45e3-94d8-06169e3c8e17"),
     6,
     {Inert::shared, L"Windows.Storage.ApplicationData"}},
    {L"Windows.Storage.ApplicationData", direct, 6, {Inert::shared, local_folder}},
    {local_folder, storage_item, 6, {Inert::console_string, L"local_folder_name"}},
    {local_folder, storage_item, 7, {Inert::console_string, L"local_folder"}},
    // public IMessageWebSocket and IWebSocket: a socket that is made and set up, and whose
    // connection fails, there being no network
    {web_socket, guid("33727d08-34d5-4746-ad7b-8dde5bc2ef88"), 6, {Inert::object, web_control}},
    {web_socket, guid("33727d08-34d5-4746-ad7b-8dde5bc2ef88"), 7, {Inert::object, web_control}},
    {web_socket, guid("33727d08-34d5-4746-ad7b-8dde5bc2ef88"), 8, {Inert::event_add}},
    {web_socket, guid("33727d08-34d5-4746-ad7b-8dde5bc2ef88"), 9, {Inert::accept}},
    {web_socket, guid("f877396f-99b1-4e18-bc08-850c9adf156e"), 6, {Inert::zero64}},
    {web_socket,
     guid("f877396f-99b1-4e18-bc08-850c9adf156e"),
     7,
     {Inert::failed_action_at, nullptr, 2, static_cast<long>(0x800704d0)}},
    {web_socket, guid("f877396f-99b1-4e18-bc08-850c9adf156e"), 8, {Inert::accept}},
    {web_socket, guid("f877396f-99b1-4e18-bc08-850c9adf156e"), 9, {Inert::event_add}},
    {web_socket, guid("f877396f-99b1-4e18-bc08-850c9adf156e"), 10, {Inert::accept}},
    {web_socket, guid("f877396f-99b1-4e18-bc08-850c9adf156e"), 11, {Inert::accept}},
    {web_control, direct, 6, {Inert::zero32}},
    {web_control, direct, 7, {Inert::accept}},
    {web_control, direct, 8, {Inert::zero32}},
    {web_control, direct, 9, {Inert::accept}},
    {web_control, guid("8118388a-11ab-4e11-8ea4-0e73e8bb0ef2"), 6, {Inert::zero32}},
    {web_control, guid("8118388a-11ab-4e11-8ea4-0e73e8bb0ef2"), 7, {Inert::accept}},
    {web_control, guid("8118388a-11ab-4e11-8ea4-0e73e8bb0ef2"), 8, {Inert::zero32}},
    {web_control, guid("8118388a-11ab-4e11-8ea4-0e73e8bb0ef2"), 9, {Inert::accept}},
    // public IKeyboardCapabilities: KeyboardPresent, the host's keyboard is there
    {L"Windows.Devices.Input.KeyboardCapabilities",
     guid("3a3f9b56-6798-4bbc-833e-0f34b17c65ff"),
     6,
     {Inert::number32, nullptr, 1}},
    // public IMouseCapabilities: no mouse
    {L"Windows.Devices.Input.MouseCapabilities", direct, 6, {Inert::zero32}},
    {L"Windows.Devices.Input.MouseCapabilities",
     guid("56cb2884-282d-49f7-a1bc-456ecc2e5de8"),
     6,
     {Inert::zero32}},
    {L"Windows.Devices.Input.MouseCapabilities",
     guid("56cb2884-282d-49f7-a1bc-456ecc2e5de8"),
     7,
     {Inert::zero32}},
    {L"Windows.Devices.Input.MouseCapabilities",
     guid("56cb2884-282d-49f7-a1bc-456ecc2e5de8"),
     8,
     {Inert::zero32}},
    {L"Windows.Devices.Input.MouseCapabilities",
     guid("56cb2884-282d-49f7-a1bc-456ecc2e5de8"),
     9,
     {Inert::zero32}},
    {L"Windows.Devices.Input.MouseCapabilities",
     guid("56cb2884-282d-49f7-a1bc-456ecc2e5de8"),
     10,
     {Inert::zero32}},

    // IControllerStatics: ControllerOrderChanged; IController::Type of the one gamepad
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     13,
     {Inert::event_add}},
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     14,
     {Inert::accept}},
    {L"Windows.Xbox.Input.Gamepad",
     guid("9776ff56-9bfd-4581-ad45-b645bba526d6"),
     7,
     {Inert::string, L"Windows.Xbox.Input.Gamepad"}},
    // IPackageTransferManagerStatics::Current
    {transfer_manager,
     guid("d6cf4cae-9eb1-4cbe-b426-7e75d429f846"),
     6,
     {Inert::shared, transfer_manager}},
    // Create(package): the one there is
    {transfer_manager,
     guid("d6cf4cae-9eb1-4cbe-b426-7e75d429f846"),
     7,
     {Inert::shared2, transfer_manager}},
    // IsChunkInstalled(chunk, bool *): the whole package is there
    {transfer_manager, direct, 7, {Inert::true8_at, nullptr, 2}},
    // AreChunksInstalled(chunks, bool *), likewise
    {transfer_manager, direct, 8, {Inert::true8_at, nullptr, 2}},
    // FindChunkFromFile: the first, which a title counts from 1
    {transfer_manager, direct, 9, {Inert::one32_at, nullptr, 2}},
    // IPackageTransferManagerForChunkSpecifiers::GetInstallationState, by chunks and by
    // specifiers: what is asked about is installed
    {transfer_manager,
     guid("96130573-d1ad-468c-aa25-9967781fb2f3"),
     7,
     {Inert::zero32_at, nullptr, 2}},
    {transfer_manager,
     guid("96130573-d1ad-468c-aa25-9967781fb2f3"),
     8,
     {Inert::zero32_at, nullptr, 2}},
    {transfer_manager, direct, 10, {Inert::zero32}}, // read as 4 bytes; zero: nothing in transfer
    // ISpeechRecognizerFactory::Create(language, SpeechRecognizer **)
    {L"Windows.Xbox.Speech.Recognition.SpeechRecognizerFactory",
     guid("9d99295c-ecd0-44e5-9f3d-6130acff3a18"),
     6,
     {Inert::lookup, speech_recognizer}},
    // public IGestureRecognizer: GestureSettings set
    {L"Windows.UI.Input.GestureRecognizer",
     guid("b47a37bf-3d6b-4f88-83e8-6dcb4012ffb0"),
     7,
     {Inert::accept}},
    {L"Windows.UI.Input.GestureRecognizer",
     guid("b47a37bf-3d6b-4f88-83e8-6dcb4012ffb0"),
     56,
     {Inert::event_add}},
    {L"Windows.UI.Input.GestureRecognizer",
     guid("b47a37bf-3d6b-4f88-83e8-6dcb4012ffb0"),
     58,
     {Inert::event_add}},
    {L"Windows.UI.Input.GestureRecognizer",
     guid("b47a37bf-3d6b-4f88-83e8-6dcb4012ffb0"),
     62,
     {Inert::event_add}},
    // IBodyControllerStatics::BodyControllers: no sensor, nobody seen
    {L"Windows.Xbox.Input.BodyController",
     guid("99ff8b9b-c70e-496a-a23b-ec4b7996b8ff"),
     6,
     {Inert::empty_list}},
    {speech_recognizer, direct, 14, {Inert::accept}}, // (bool)
    {speech_recognizer, direct, 17, {Inert::event_add}},
    // the same event through the recognizer's interface by name
    {speech_recognizer, guid("3a3aa19c-f4e9-4985-a53c-dbcc22795ca7"), 17, {Inert::event_add}},
    {speech_recognizer, direct, 21, {Inert::event_add}},
    {speech_recognizer, direct, 23, {Inert::event_add}},
    {speech_recognizer, direct, 25, {Inert::event_add}},
    {speech_recognizer, direct, 27, {Inert::event_add}},
    {speech_recognizer, direct, 29, {Inert::event_add}},
    {speech_recognizer, direct, 31, {Inert::event_add}},
    {transport, game_transport_controls, 26, {Inert::accept}}, // button pressed, removed
    // IUserStatics::GetUserById(id, User **): the one user there is
    {user, user_statics, 14, {Inert::shared2, user}},
    // KinectSensor's method 9, by a title's call of it (image+0x9fd9d4): no argument, and
    // only the HRESULT is looked at. There is no sensor, and nothing comes of it.
    {L"Windows.Kinect.KinectSensor", direct, 9, {Inert::accept}},
    // The public IKinectSensor::BodyIndexFrameSource and what a title asks of it: the size
    // of the frames it would give, a reader that never has one
    {L"Windows.Kinect.KinectSensor",
     direct,
     15,
     {Inert::object, L"Windows.Kinect.BodyIndexFrameSource"}},
    {L"Windows.Kinect.BodyIndexFrameSource", direct, 6, {Inert::event_add}},
    {L"Windows.Kinect.BodyIndexFrameSource", direct, 8, {Inert::zero8}}, // IsActive
    {L"Windows.Kinect.BodyIndexFrameSource",
     direct,
     9,
     {Inert::object, L"Windows.Kinect.BodyIndexFrameReader"}},
    {L"Windows.Kinect.BodyIndexFrameSource",
     direct,
     10,
     {Inert::object, L"Windows.Kinect.FrameDescription"}},
    {L"Windows.Kinect.BodyIndexFrameReader", direct, 6, {Inert::event_add}},
    {L"Windows.Kinect.BodyIndexFrameReader", direct, 8, {Inert::zero64}},             // no frame
    {L"Windows.Kinect.FrameDescription", direct, 6, {Inert::number32, nullptr, 512}}, // Width
    {L"Windows.Kinect.FrameDescription", direct, 7, {Inert::number32, nullptr, 424}}, // Height
    {L"Windows.Kinect.FrameDescription", direct, 11, {Inert::number32, nullptr, 512 * 424}},
    {L"Windows.Kinect.FrameDescription", direct, 12, {Inert::number32, nullptr, 1}},
    {L"Windows.Kinect.KinectSensor", direct, 22, {Inert::zero32}}, // KinectCapabilities: none
    // IConfigurationStatics::MarketplaceId
    {L"Windows.Xbox.ApplicationModel.Store.Configuration",
     guid("9d50b2e8-eeb6-4ced-b9f1-3eb95b3c1e0f"),
     6,
     {Inert::string, L"US"}},
    // public IRatedContentRestrictions and IRatedContentDescriptionFactory: nothing is
    // restricted, and that never changes
    {L"Windows.Media.ContentRestrictions.RatedContentRestrictions",
     guid("3f7f23cb-ba07-4401-a49d-8b9222205723"),
     7,
     {Inert::zero_operation_at, nullptr, 2}}, // GetRestrictionLevelAsync: Allow
    {L"Windows.Media.ContentRestrictions.RatedContentRestrictions",
     guid("3f7f23cb-ba07-4401-a49d-8b9222205723"),
     9,
     {Inert::event_add}},
    {L"Windows.Media.ContentRestrictions.RatedContentDescription",
     guid("2e38df62-9b90-4fa6-89c1-4b8d2ffb3573"),
     6,
     {Inert::object_at, L"Windows.Media.ContentRestrictions.RatedContentDescription", 4}},
    // the description's setters: Id, Title, Image, Category, Ratings
    {L"Windows.Media.ContentRestrictions.RatedContentDescription", direct, 7, {Inert::accept}},
    {L"Windows.Media.ContentRestrictions.RatedContentDescription", direct, 9, {Inert::accept}},
    {L"Windows.Media.ContentRestrictions.RatedContentDescription", direct, 11, {Inert::accept}},
    {L"Windows.Media.ContentRestrictions.RatedContentDescription", direct, 13, {Inert::accept}},
    {L"Windows.Media.ContentRestrictions.RatedContentDescription", direct, 15, {Inert::accept}},
    // Body's statics, the public IBodyStatics: JointCount
    {L"Windows.Kinect.Body",
     guid("38d92488-eecf-4e85-a03a-545fbfa362a9"),
     6,
     {Inert::number32, nullptr, 25}},
    // IDisplayConfigurationStatics2::TrySetHdrModeAsync, by a title's symbols: an operation
    // that gives an HdrModeInfo. A title waits for it before it creates its device.
    {L"Windows.Xbox.Graphics.Display.DisplayConfiguration",
     guid("081b331e-db52-49ed-ba5f-2c656b14b232"),
     6,
     {Inert::operation, L"Windows.Xbox.Graphics.Display.HdrModeInfo"}},
    // its first property, a bool (image+0x3afef1): the display is not in an HDR mode
    {L"Windows.Xbox.Graphics.Display.HdrModeInfo", direct, 6, {Inert::zero8}},
    // IAppModelUtil's first method, by the dashboard's call of it (dashboard.dll+0x1f1fe51):
    // (bool), and only the HRESULT is looked at
    {L"Windows.Xbox.System.Internal.Interop.AppModelUtil",
     guid("ddd0e604-011e-4d2f-bf24-df3486cef6b3"),
     6,
     {Inert::accept}},
};
} // namespace

const InertMethod *inert_method(const wchar_t *type, const GUID &iid, std::size_t method) {
    for (const auto &row : rows)
        if (row.method == method && row.iid == iid && !std::wcscmp(row.type, type))
            return &row.behavior;
    return nullptr;
}
} // namespace xv::runtime::durango::winrt
