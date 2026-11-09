// SPDX-FileCopyrightText: 2026 andrew
// SPDX-License-Identifier: BSD-3-Clause
#include "store.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/desktop/view/window/WindowBackend.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/config/lua/LuaCaller.hpp>
#include <hyprland/src/config/lua/objects/LuaWindow.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <initializer_list>
#include <unordered_map>

namespace {
// Use raw lookups: user metatables must not run outside a protected Lua call.
void field(lua_State *L, int table, const char *name) {
    table = lua_absindex(L, table);
    lua_pushstring(L, name);
    lua_rawget(L, table);
}
void fields(lua_State *L, int table, std::initializer_list<std::string_view> allowed) {
    table = lua_absindex(L, table);
    lua_pushnil(L);
    while (lua_next(L, table)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            throw std::runtime_error("option keys must be strings");
        std::string_view key = lua_tostring(L, -2);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::runtime_error("unknown option: " + std::string(key));
        lua_pop(L, 1);
    }
}
std::string textField(lua_State *L, int table, const char *name, std::string fallback) {
    field(L, table, name);
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING)
            throw std::runtime_error(std::string(name) + " must be a string");
        size_t length = 0;
        const char *value = lua_tolstring(L, -1, &length);
        if (!length || length > 4096)
            throw std::runtime_error(std::string(name) + " must contain 1..4096 bytes");
        fallback.assign(value, length);
    }
    lua_pop(L, 1);
    return fallback;
}
bool boolField(lua_State *L, int table, const char *name, bool fallback) {
    field(L, table, name);
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1))
            throw std::runtime_error(std::string(name) + " must be boolean");
        fallback = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return fallback;
}
memory::Geometry sizeField(lua_State *L, int table, memory::Geometry fallback) {
    field(L, table, "default_size");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            throw std::runtime_error("default_size must be {width=..., height=...}");
        fields(L, -1, {"width", "height"});
        const int sizeTable = lua_gettop(L);
        field(L, sizeTable, "width");
        field(L, sizeTable, "height");
        if (!lua_isinteger(L, -2) || !lua_isinteger(L, -1))
            throw std::runtime_error("width and height must be integers");
        const auto w = lua_tointeger(L, -2), h = lua_tointeger(L, -1);
        if (w < 1 || w > 32768 || h < 1 || h > 32768)
            throw std::runtime_error("dimensions must be in 1..32768");
        fallback = {static_cast<int>(w), static_cast<int>(h), 0, {}};
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    return fallback;
}
struct Policy {
    memory::Key key;
    memory::Geometry defaultSize;
    bool center = true;
    bool rememberPosition = true;
    bool enabled = true;
};
memory::Geometries loadSizes() {
    auto result = memory::loadState();
    if (!result)
        throw std::runtime_error(result.error());
    return std::move(*result);
}
struct Entry {
    Policy policy;
    std::optional<memory::Geometry> last;
    UP<SEventLoopDoLaterLock> restore;
};
class Plugin {
  public:
    explicit Plugin(HANDLE handle) : handle_(handle), sizes_(loadSizes()), writer_(memory::statePath(), sizes_) {
        retryTimer_ = makeShared<CEventLoopTimer>(
            std::nullopt,
            [this](SP<CEventLoopTimer>, void *) {
                serviceWriter();
                reportError();
            },
            nullptr);
        g_pEventLoopManager->addTimer(retryTimer_);
        auto &events = Event::bus()->m_events;
        opened_ = events.window.openLate.listen([this](PHLWINDOW w) { schedule(w); });
        floated_ = events.window.floating.listen([this](PHLWINDOW w) {
            if (w->isFloating())
                schedule(w);
            else if (auto it = entries_.find(w->metadata().stableID()); it != entries_.end())
                it->second.restore.reset();
        });
        closed_ = events.window.close.listen([this](PHLWINDOW w) {
            entries_.erase(w->metadata().stableID());
            writer_.flushAsync();
            serviceWriter();
            reportError();
        });
        exited_ = events.exit.listen([this] {
            (void)writer_.flush();
            reportError();
        });
        auto &drag = g_layoutManager->dragController()->m_events;
        motion_ = drag.motion.listen([this] { recordDrag(); });
        ended_ = drag.ended.listen([this] {
            writer_.flushAsync();
            serviceWriter();
            reportError();
        });
        reloaded_ = events.config.reloaded.listen([this] {
            // A failed syntax check keeps the old Lua state alive. Preserve its config.
            // A successful reload without configure() resets to the built-in defaults.
            if (configLifetime_ && !configLifetime_->state) {
                rule_ = {};
                configLifetime_.reset();
                defaultSize_ = {};
                center_ = true;
                rememberPosition_ = true;
                entries_.clear();
            }
        });
    }
    ~Plugin() {
        opened_.reset();
        floated_.reset();
        closed_.reset();
        exited_.reset();
        motion_.reset();
        ended_.reset();
        reloaded_.reset();
        retryTimer_->cancel();
        g_pEventLoopManager->removeTimer(retryTimer_);
        retryTimer_.reset();
        entries_.clear(); // Cancels queued restores before unloading the DSO.
        (void)writer_.flush();
        reportError();
    }
    void configure(lua_State *L) {
        if (evaluating_)
            throw std::runtime_error("configure cannot be called from rule");
        if (!lua_istable(L, 1))
            throw std::runtime_error("configure expects a table");
        fields(L, 1, {"default_size", "center", "remember_position", "rule"});
        const auto size = sizeField(L, 1, {});
        const auto center = boolField(L, 1, "center", true);
        const auto rememberPosition = boolField(L, 1, "remember_position", true);
        auto *manager = Config::Lua::CConfigManager::fromLuaState(L);
        if (!manager)
            throw std::runtime_error("missing Hyprland Lua state");
        field(L, 1, "rule");
        Config::Lua::CLuaRef rule;
        if (!lua_isnil(L, -1)) {
            if (!lua_isfunction(L, -1))
                throw std::runtime_error("rule must be a function");
            rule = Config::Lua::CLuaRef(manager->luaStateLifetime(), L, -1);
        }
        lua_pop(L, 1);
        // Commit only after the whole configuration has passed validation.
        rule_ = std::move(rule);
        configLifetime_ = manager->luaStateLifetime();
        defaultSize_ = size;
        center_ = center;
        rememberPosition_ = rememberPosition;
        entries_.clear();
        lastRuleError_.clear();
    }

  private:
    bool eligible(PHLWINDOW w) const {
        return w && w->mapped() && w->isFloating() && !w->backend().traits().overrideRedirect &&
               !Fullscreen::controller()->isFullscreen(w) && w->layoutTarget() && w->layoutTarget()->space();
    }
    void track(PHLWINDOW w) {
        if (!w || w->backend().traits().overrideRedirect || evaluating_)
            return;
        const auto &meta = w->metadata();
        if (entries_.contains(meta.stableID()))
            return;
        const auto &app = meta.initialAppID().empty() ? meta.appID() : meta.initialAppID();
        Policy policy{memory::identity(app, w->backend().traits().transient), defaultSize_, center_, rememberPosition_,
                      !app.empty()};
        if (rule_.state()) {
            std::string failure;
            evaluating_ = true;
            auto *manager = Config::Lua::CConfigManager::fromLuaState(rule_.state());
            const auto result = manager->caller().call(
                rule_, 50, "float_memory.rule",
                [w](lua_State *L) {
                    Config::Lua::Objects::CLuaWindow::push(L, w);
                    return 1;
                },
                [&](lua_State *L) {
                    const int top = lua_gettop(L);
                    try {
                        if (lua_isnil(L, -1))
                            return;
                        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
                            policy.enabled = false;
                            return;
                        }
                        if (!lua_istable(L, -1))
                            throw std::runtime_error("rule must return nil, false, or a table");
                        const int table = lua_gettop(L);
                        fields(L, table, {"key", "role", "default_size", "center", "remember_position"});
                        policy.key.first = textField(L, table, "key", policy.key.first);
                        policy.key.second = textField(L, table, "role", policy.key.second);
                        policy.defaultSize = sizeField(L, table, policy.defaultSize);
                        policy.center = boolField(L, table, "center", policy.center);
                        policy.rememberPosition = boolField(L, table, "remember_position", policy.rememberPosition);
                        policy.enabled = !policy.key.first.empty();
                    } catch (const std::exception &e) {
                        failure = e.what();
                    }
                    lua_settop(L, top);
                });
            evaluating_ = false;
            if (!result)
                failure = result.error();
            if (!failure.empty()) {
                policy.enabled = false;
                if (failure != lastRuleError_)
                    HyprlandAPI::addNotification(handle_, "float_memory.rule: " + failure,
                                                 CHyprColor{1.0, 0.3, 0.2, 1.0}, 8000);
                lastRuleError_ = failure;
            }
        }
        entries_.try_emplace(meta.stableID(), Entry{std::move(policy), {}, {}});
    }
    void schedule(PHLWINDOW w) {
        if (!eligible(w))
            return;
        track(w);
        auto it = entries_.find(w->metadata().stableID());
        if (it == entries_.end() || !it->second.policy.enabled)
            return;
        const PHLWINDOWREF weak = w;
        it->second.restore = g_pEventLoopManager->doLaterLock([this, weak] {
            if (auto window = weak.lock())
                restore(window);
        });
    }
    void restore(PHLWINDOW w) {
        if (!eligible(w))
            return;
        // A tiled window temporarily floated by a drag must retain its drag geometry.
        const auto &drag = g_layoutManager->dragController();
        if (drag->target() == w->layoutTarget())
            return;
        auto it = entries_.find(w->metadata().stableID());
        if (it == entries_.end() || !it->second.policy.enabled)
            return;
        auto &entry = it->second;
        const auto saved = sizes_.find(entry.policy.key);
        const auto wanted = entry.last.value_or(saved == sizes_.end() ? entry.policy.defaultSize : saved->second);
        auto target = w->layoutTarget();
        const auto area = target->space()->workArea(true);
        if (area.w <= 0 || area.h <= 0)
            return;
        Vector2D size{static_cast<double>(wanted.width), static_cast<double>(wanted.height)};
        size.x = std::min(size.x, area.w);
        size.y = std::min(size.y, area.h);
        if (auto minimum = target->minSize()) {
            size.x = std::max(size.x, minimum->x);
            size.y = std::max(size.y, minimum->y);
        }
        if (auto maximum = target->maxSize()) {
            if (maximum->x > 0)
                size.x = std::min(size.x, maximum->x);
            if (maximum->y > 0)
                size.y = std::min(size.y, maximum->y);
        }
        auto pos = entry.policy.center ? area.pos() + (area.size() - size) / 2.0 : target->position().pos();
        if (entry.policy.rememberPosition && wanted.position) {
            const auto aw = memory::coordinate(area.w), ah = memory::coordinate(area.h);
            const auto sw = memory::coordinate(size.x), sh = memory::coordinate(size.y);
            if (!aw || !ah || !sw || !sh || !std::isfinite(area.x) || !std::isfinite(area.y))
                return;
            const auto fitted = memory::fitPosition(*wanted.position, *sw, *sh, *aw, *ah);
            pos = area.pos() + Vector2D{static_cast<double>(fitted.x), static_cast<double>(fitted.y)};
        }
        g_layoutManager->setTargetGeom(CBox{pos, size}, target);
        target->rememberFloatingSize(size);
        auto touched = wanted;
        touched.lastUsed = memory::nowSeconds();
        sizes_[entry.policy.key] = touched;
        writer_.submit(entry.policy.key, touched);
        serviceWriter();
        // Monitor fitting is temporary: don't replace the user's preferred size.
    }
    void recordDrag() {
        const auto &drag = g_layoutManager->dragController();
        const auto mode = drag->mode();
        if (mode != MBIND_MOVE && mode != MBIND_RESIZE && mode != MBIND_RESIZE_FORCE_RATIO &&
            mode != MBIND_RESIZE_BLOCK_RATIO)
            return;
        const auto target = drag->target();
        if (!target || drag->draggingTiled())
            return;
        auto w = target->window();
        if (!eligible(w))
            return;
        track(w);
        auto it = entries_.find(w->metadata().stableID());
        if (it == entries_.end() || !it->second.policy.enabled)
            return;
        const auto &policy = it->second.policy;
        if (mode == MBIND_MOVE && !policy.rememberPosition)
            return;
        const auto size = w->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
        const auto width = memory::coordinate(size.x), height = memory::coordinate(size.y);
        if (!width || !height)
            return;
        const auto saved = sizes_.find(policy.key);
        auto actual = it->second.last.value_or(saved == sizes_.end() ? memory::Geometry{} : saved->second);
        actual.width = *width;
        actual.height = *height;
        if (policy.rememberPosition) {
            const auto area = target->space()->workArea(true);
            const auto pos = target->position().pos() - area.pos();
            const auto x = memory::coordinate(pos.x), y = memory::coordinate(pos.y);
            if (!x || !y)
                return;
            actual.position = memory::Position{*x, *y};
        }
        if (!memory::valid(actual))
            return;
        // Compare geometry before refreshing the timestamp: no-op motion is not use.
        const auto previous = it->second.last.value_or(saved == sizes_.end() ? memory::Geometry{} : saved->second);
        if (actual.width == previous.width && actual.height == previous.height && actual.position == previous.position)
            return;
        actual.lastUsed = memory::nowSeconds();
        it->second.restore.reset();
        it->second.last = actual;
        const auto &key = it->second.policy.key;
        auto [stored, inserted] = sizes_.try_emplace(key, actual);
        if (!inserted && stored->second == actual)
            return;
        stored->second = actual;
        writer_.submit(key, actual);
        serviceWriter();
        reportError();
    }
    void serviceWriter() {
        writer_.pump();
        if (writer_.needsPump())
            retryTimer_->updateTimeout(std::chrono::milliseconds(5));
        else if (retryTimer_->armed())
            retryTimer_->updateTimeout(std::nullopt);
    }
    void reportError() {
        if (!writer_.hasError()) {
            lastError_.clear();
            return;
        }
        const auto error = writer_.error();
        if (!error.empty() && error != lastError_)
            HyprlandAPI::addNotification(handle_, "hypr-float-memory: " + error, CHyprColor{1.0, 0.3, 0.2, 1.0}, 8000);
        lastError_ = error;
    }
    HANDLE handle_;
    memory::Geometries sizes_;
    memory::Writer writer_;
    SP<CEventLoopTimer> retryTimer_;
    std::string lastError_, lastRuleError_;
    memory::Geometry defaultSize_;
    bool center_ = true, rememberPosition_ = true, evaluating_ = false;
    Config::Lua::CLuaRef rule_;
    SP<Config::Lua::SLuaStateLifetime> configLifetime_;
    std::unordered_map<uint64_t, Entry> entries_;
    CHyprSignalListener opened_, floated_, closed_, exited_, motion_, ended_, reloaded_;
};
std::unique_ptr<Plugin> plugin;
int configureLua(lua_State *L) {
    try {
        if (!plugin)
            throw std::runtime_error("plugin is unavailable");
        plugin->configure(L);
        return 0;
    } catch (const std::exception &e) {
        lua_pushstring(L, e.what());
    }
    // No C++ objects requiring destruction across Lua's longjmp.
    return lua_error(L);
}
} // namespace
APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    plugin = std::make_unique<Plugin>(handle);
    if (!HyprlandAPI::addLuaFunction(handle, "float_memory", "configure", configureLua)) {
        plugin.reset();
        throw std::runtime_error("could not register float_memory.configure");
    }
    return {"hypr-float-memory", "Remember manually resized floating windows across sessions", "andrew", "0.1.0"};
}
APICALL EXPORT void PLUGIN_EXIT() { plugin.reset(); }
