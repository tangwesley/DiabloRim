// =============================================================================
//  Swallowing one HUD notification. See Notify.h.
// =============================================================================

#include "PCH.h"

#include "Notify.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    struct Armed
    {
        std::string                           text;
        std::chrono::steady_clock::time_point at{};
    };

    std::mutex         g_lock;
    std::vector<Armed> g_armed;

    // ★AN ATOMIC COUNT ALONGSIDE THE VECTOR, so the hook can decline in one
    // relaxed load. ProcessMessage runs for every HUD message the game sends --
    // subtitles, crosshair updates, meters, all of it -- and taking a mutex on
    // each one to discover there is nothing to do would be a cost paid forever
    // for a feature used a handful of times a playthrough.
    std::atomic<std::size_t> g_armedCount{ 0 };

    // Long enough to cover the settle window and the roll, short enough that a
    // mis-armed entry cannot swallow something the player wanted much later.
    constexpr auto kExpiry = std::chrono::seconds{ 5 };

    std::atomic<bool> g_installed{ false };

    // Whether this text is one we were asked to swallow. Disarms on a match, so
    // a single arming eats exactly one message.
    bool ClaimMatch(std::string_view a_text)
    {
        const auto       now = std::chrono::steady_clock::now();
        std::scoped_lock lock{ g_lock };

        bool claimed = false;
        for (auto it = g_armed.begin(); it != g_armed.end();) {
            if (now - it->at >= kExpiry) {
                it = g_armed.erase(it);  // expired, and nothing matched it
                continue;
            }
            if (!claimed && a_text.find(it->text) != std::string_view::npos) {
                claimed = true;
                it = g_armed.erase(it);
                continue;
            }
            ++it;
        }

        g_armedCount.store(g_armed.size(), std::memory_order_relaxed);
        return claimed;
    }

    struct HUDHook
    {
        static RE::UI_MESSAGE_RESULTS ProcessMessage(RE::IMenu* a_this, RE::UIMessage& a_message)
        {
            // ★THE CHEAP TEST FIRST, ALWAYS. Nothing below runs in the common
            // case, which is every frame of every game that never asked for a
            // notification to be swallowed.
            if (g_armedCount.load(std::memory_order_relaxed) == 0 ||
                a_message.type != RE::UI_MESSAGE_TYPE::kUpdate || !a_message.data) {
                return _ProcessMessage(a_this, a_message);
            }

            // skyrim_cast, not a static_cast: IUIMessageData is a base shared by
            // several payloads and the HUD receives more than one of them. An
            // unchecked cast here would read another message's fields as a
            // HUDData, which is a crash waiting for the right frame.
            auto* hud = skyrim_cast<RE::HUDData*>(a_message.data);
            if (!hud || hud->type != RE::HUD_MESSAGE_TYPE::kNotification) {
                return _ProcessMessage(a_this, a_message);
            }

            const char* text = hud->text.c_str();
            if (!text || !*text) {
                return _ProcessMessage(a_this, a_message);
            }

            if (!ClaimMatch(text)) {
                return _ProcessMessage(a_this, a_message);
            }

            logger::debug("notify: swallowed '{}'", text);

            // kHandled, and the original is NOT called: the message stops here.
            return RE::UI_MESSAGE_RESULTS::kHandled;
        }

        static inline REL::Relocation<decltype(ProcessMessage)> _ProcessMessage;
    };
}

void Notify::Install()
{
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;
    }

    REL::Relocation<std::uintptr_t> vtbl{ RE::HUDMenu::VTABLE[0] };
    HUDHook::_ProcessMessage = vtbl.write_vfunc(0x04, HUDHook::ProcessMessage);
    logger::info("notify: HUD notification filter installed");
}

void Notify::MuteNext(std::string_view a_text)
{
    if (a_text.empty()) {
        return;
    }

    std::scoped_lock lock{ g_lock };
    g_armed.push_back({ std::string{ a_text }, std::chrono::steady_clock::now() });
    g_armedCount.store(g_armed.size(), std::memory_order_relaxed);
}
