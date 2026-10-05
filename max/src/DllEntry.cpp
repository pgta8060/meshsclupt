// Sculpt Mesh for 3ds Max — DLL entry points.
#include <mutex>
#include <string>
#include <unordered_map>

#include <notify.h>
#include <systemutilities.h>

#include "SculptMeshPlugin.h"
#include "SculptMode.h"

HINSTANCE hInstance = nullptr;

namespace {

// Leave sculpt mode before the scene goes away so no pointer outlives it.
void OnSceneReset(void* /*param*/, NotifyInfo* /*info*/) { SculptMode::Get().Stop(); }

const int kSceneNotifications[] = {NOTIFY_SYSTEM_PRE_RESET, NOTIFY_SYSTEM_PRE_NEW, NOTIFY_FILE_PRE_OPEN,
                                   NOTIFY_SYSTEM_SHUTDOWN};

bool notificationsRegistered = false;

}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, ULONG fdwReason, LPVOID /*lpvReserved*/) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        MaxSDK::Util::UseLanguagePackLocale();
        hInstance = hinstDLL;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}

__declspec(dllexport) const TCHAR* LibDescription() { return GetString(IDS_LIB_DESCRIPTION); }

__declspec(dllexport) int LibNumberClasses() { return 1; }

__declspec(dllexport) ClassDesc* LibClassDesc(int i) {
    switch (i) {
        case 0:
            return GetSculptMeshObjectDesc();
        default:
            return nullptr;
    }
}

__declspec(dllexport) ULONG LibVersion() { return VERSION_3DSMAX; }

// The plugin defines MAXScript functions (convertToSculpt, SculptMesh.*),
// so it must load at startup rather than on first use.
__declspec(dllexport) ULONG CanAutoDefer() { return 0; }

__declspec(dllexport) int LibInitialize() {
    if (!notificationsRegistered) {
        for (int code : kSceneNotifications) RegisterNotification(OnSceneReset, nullptr, code);
        notificationsRegistered = true;
    }
    return TRUE;
}

__declspec(dllexport) int LibShutdown() {
    if (notificationsRegistered) {
        for (int code : kSceneNotifications) UnRegisterNotification(OnSceneReset, nullptr, code);
        notificationsRegistered = false;
    }
    return TRUE;
}

// Each string is loaded once and cached, so returned pointers stay valid for
// the life of the plugin (3ds Max may keep ClassName()/Category() pointers).
const TCHAR* GetString(int id) {
    static std::mutex mutex;
    static std::unordered_map<int, std::basic_string<TCHAR>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(id);
    if (it == cache.end()) {
        TCHAR buf[512] = {};
        const int length = hInstance ? LoadString(hInstance, id, buf, static_cast<int>(_countof(buf))) : 0;
        it = cache.emplace(id, std::basic_string<TCHAR>(buf, length > 0 ? static_cast<std::size_t>(length) : 0u)).first;
    }
    return it->second.c_str();
}
