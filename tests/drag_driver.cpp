// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
// Loaded only by isolated integration tests.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <stdexcept>
static SP<CEventLoopTimer> timer;
static void drag(bool move, bool topLeft, double dx, double dy) {
    if (timer) {
        timer->cancel();
        g_pEventLoopManager->removeTimer(timer);
        timer.reset();
    }
    for (const auto &w : Desktop::windowState()->windows()) {
        if (w->metadata().appID() != "hfm-integration-test" || !w->isFloating())
            continue;
        const auto mouse = g_pInputManager->getMouseCoordsInternal();
        g_layoutManager->beginDragTarget(w->layoutTarget(), move ? MBIND_MOVE : MBIND_RESIZE,
                                         topLeft ? Layout::CORNER_TOPLEFT : Layout::CORNER_BOTTOMRIGHT);
        timer = makeShared<CEventLoopTimer>(
            std::chrono::milliseconds(30),
            [mouse, dx, dy, i = 0](SP<CEventLoopTimer> self, void *) mutable {
                ++i;
                g_layoutManager->moveMouse(mouse + Vector2D{dx * i / 10, dy * i / 10});
                if (i == 10) {
                    g_layoutManager->endDragTarget();
                    self->updateTimeout(std::nullopt);
                } else
                    self->updateTimeout(std::chrono::milliseconds(30));
            },
            nullptr);
        g_pEventLoopManager->addTimer(timer);
        return;
    }
    throw std::runtime_error("No integration test floating window");
}
APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    if (!HyprlandAPI::addLuaFunction(handle, "float_memory_test", "drag", [](lua_State *L) {
            drag(lua_toboolean(L, 1), lua_toboolean(L, 2), lua_tonumber(L, 3), lua_tonumber(L, 4));
            return 0;
        }))
        throw std::runtime_error("test driver registration failed");
    drag(false, false, -120, -80);
    return {"hfm-test-driver", "Integration test drag controller", "test", "1"};
}
APICALL EXPORT void PLUGIN_EXIT() {
    if (timer) {
        timer->cancel();
        g_pEventLoopManager->removeTimer(timer);
        timer.reset();
    }
}
