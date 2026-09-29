#include <atomic>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../activity/mission_launch.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * The client's "loading cinematics are suppressed" wrapper: `sub rsp,28; call <stub>; test al,al;
 * setne al; add rsp,28; ret`, the stub being retail's compiled-out `return 0`. The image's
 * xchg/lea/jmp filler after the `ret` makes the pattern unique. Its callers are the whole loading
 * presentation of a transition: the loading UI, the cinematic preload, the fly-in batch and the
 * orbit outro all skip their movies when it answers true.
 */
constexpr std::string_view kSuppressedSignatureText =
    "48 83 EC 28 E8 ? ? ? ? 84 C0 0F 95 C0 48 83 C4 28 C3 48 87 2C 24 48 8D 64 24 08 FF 64 24 F8";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kSuppressedSignature =
    signature<signature_length(kSuppressedSignatureText)>(kSuppressedSignatureText);

using Suppressed = bool(__fastcall*)() noexcept;

std::atomic<Suppressed> g_original{nullptr};
hooking::detour::Handle g_handle{};

/** Answers "suppressed" while an activity continuation is between its exit and its arrival. */
bool __fastcall loading_cinematics_suppressed() noexcept {
    if (activity::mission_launch::suppress_loading()) {
        return true;
    }
    const Suppressed original = g_original.load(std::memory_order_acquire);
    return original != nullptr && original();
}

} // namespace

/** Detours the wrapper. A miss only costs a continuation its loading-screen presentation. */
bool install_loading_suppression() noexcept {
    std::byte* const target = scan_main_image_unique(kSuppressedSignature, "loading_suppressed");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=loading_suppression result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&loading_cinematics_suppressed)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=loading_suppression result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<Suppressed>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=loading_suppression result=ok");
    return true;
}

/** Detaches the wrapper detour. */
void uninstall_loading_suppression() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
