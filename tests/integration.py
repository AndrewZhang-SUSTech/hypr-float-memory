#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 andrew
# SPDX-License-Identifier: BSD-3-Clause
"""Opt-in nested Wayland integration test. Never connects to the main IPC socket.
Requires Hyprland, kitty, a running Wayland session and the test driver target.
Usage: python3 tests/integration.py build
"""
import glob
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from cbor_fixture import encode, decode, geometry

build = Path(sys.argv[1]).resolve()
plugin = build / 'hypr-float-memory.so'
driver = build / 'hfm-test-driver.so'
assert plugin.is_file() and driver.is_file()
parent_display = os.environ['WAYLAND_DISPLAY']
if not parent_display.startswith('/'):
    parent_display = str(Path(os.environ['XDG_RUNTIME_DIR']) / parent_display)


def wait_for(fn, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = fn()
        if value:
            return value
        time.sleep(.1)
    raise RuntimeError('Timed out: ' + repr(fn))


with tempfile.TemporaryDirectory(prefix='hf-') as root:
    root = Path(root)
    state = root / 'state'
    expected = None
    settings = root / 'config/hypr-float-memory'
    settings.mkdir(parents=True)
    (settings / 'config.toml').write_text('[retention]\nmax_age_days = 1\n')
    state_file = state / 'hypr-float-memory/sizes.cbor'
    state_file.parent.mkdir(parents=True)
    now = int(time.time())
    state_file.write_bytes(encode([2, [['expired', 'main', 600, 400, None, now-2*86400], ['fresh', 'main', 600, 400, None, now]]]))
    for attempt in range(2):
        runtime = root / f'r{attempt}'
        runtime.mkdir(mode=0o700)
        config = root / 'hyprland.lua'
        config.write_text('''hl.monitor({output="",mode="1280x800@60",position="auto",scale=1})
hl.config({binds={drag_threshold=0},animations={enabled=false},general={gaps_in=0,gaps_out=0},misc={disable_hyprland_logo=true,disable_splash_rendering=true}})
''')
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_STATE_HOME=str(state), XDG_CONFIG_HOME=str(root / "config"), XDG_CACHE_HOME=str(root / 'cache'),
                   WAYLAND_DISPLAY=parent_display, HYPRLAND_NO_SD_VARS='1', AQ_DRM_DEVICES='/dev/nonexistent')
        with open(root / f'compositor-{attempt}.log', 'w') as log:
            compositor = subprocess.Popen(['Hyprland', '--config', str(config)], env=env, stdout=log, stderr=log)
            client = None
            try:
                ipc = wait_for(lambda: next(iter(glob.glob(str(runtime / 'hypr/*/.socket.sock'))), None))
                def command(cmd):
                    with socket.socket(socket.AF_UNIX) as s:
                        s.settimeout(5)
                        s.connect(ipc)
                        s.sendall(cmd.encode())
                        s.shutdown(socket.SHUT_WR)
                        data = bytearray()
                        while part := s.recv(65536):
                            data.extend(part)
                        return data.decode()
                def ok(cmd):
                    result = command(cmd)
                    assert result.strip() == 'ok', (cmd, result)
                def windows():
                    return [w for w in json.loads(command('j/clients')) if w['class'] == 'hfm-integration-test']
                def toggle():
                    ok('eval hl.dispatch(hl.dsp.window.float({action="toggle"}))')
                    time.sleep(.2)
                ok('plugin load ' + str(plugin))
                time.sleep(1)  # Let the parent compositor configure the nested output.
                assert all(row[0] != 'expired' for row in decode(state_file.read_bytes())[1])
                assert any(row[0] == 'fresh' for row in decode(state_file.read_bytes())[1])
                display = wait_for(lambda: next((p for p in runtime.glob('wayland-*') if not p.name.endswith('.lock')), None))
                client_env = dict(env, WAYLAND_DISPLAY=str(display))
                client = subprocess.Popen(['kitty', '--config', 'NONE', '--override', 'remember_window_size=no', '--class', 'hfm-integration-test', 'sleep', '90'], env=client_env, stdout=log, stderr=log)
                wait_for(windows)
                ok('eval hl.dispatch(hl.dsp.window.fullscreen_state({internal=0,client=0}))')
                toggle()
                first = windows()[0]
                assert first['floating']
                if attempt == 0:
                    assert first['size'][1] == 720, first
                    assert 0 < first['size'][0] <= 1200, first
                    ok('plugin load ' + str(driver))
                    time.sleep(.5)
                    expected = windows()[0]['size']
                    print('manual size:', expected, flush=True)
                    assert expected != first['size'], (expected, first['size'])
                    resized_pos = windows()[0]['at']
                    ok('eval hl.plugin.float_memory_test.drag(true,false,30,20)')
                    time.sleep(.5)
                    assert windows()[0]['at'] != resized_pos, windows()[0]
                    moved = windows()[0]['at']
                    ok('eval hl.plugin.float_memory_test.drag(false,true,20,15)')
                    time.sleep(.5)
                    expected = windows()[0]['size']
                    expected_pos = windows()[0]['at']
                    assert expected_pos != moved, (moved, expected_pos)
                    print('manual geometry:',expected,expected_pos,flush=True)
                    saved = wait_for(lambda: (state / 'hypr-float-memory/sizes.cbor').exists())
                    data = geometry((state / 'hypr-float-memory/sizes.cbor').read_bytes())
                    # Change the isolated output origin and scale; fitting must not rewrite preferences.
                    config.write_text(config.read_text().replace('position="auto"','position="200x100"'))
                    ok('reload'); time.sleep(.5)
                    toggle(); toggle()
                    assert windows()[0]['at'] == [expected_pos[0]+200,expected_pos[1]+100], windows()[0]
                    assert geometry(state_file.read_bytes()) == data
                    config.write_text(config.read_text().replace('scale=1','scale=2'))
                    ok('reload'); time.sleep(.5)
                    toggle(); toggle()
                    fitted = windows()[0]
                    assert fitted['at'] == [200,100], fitted
                    assert fitted['size'][0] <= 640 and fitted['size'][1] <= 400, fitted
                    assert geometry(state_file.read_bytes()) == data
                    config.write_text(config.read_text().replace('position="200x100"','position="auto"').replace('scale=2','scale=1'))
                    ok('reload'); time.sleep(.5)
                    print('PASS: relative origin, work-area shrinking, preference preservation',flush=True)

                    toggle()
                    toggle()
                    assert windows()[0]['size'] == expected, (windows()[0], expected)
                    assert windows()[0]['at'] == expected_pos, (windows()[0], expected_pos)
                    ok('eval hl.dispatch(hl.dsp.window.fullscreen({action="set"}))')
                    time.sleep(.2)
                    ok('eval hl.dispatch(hl.dsp.window.fullscreen({action="unset"}))')
                    time.sleep(.2)
                    assert geometry((state / 'hypr-float-memory/sizes.cbor').read_bytes()) == data
                    ok('plugin unload ' + str(driver))
                    print('PASS: first float, manual resize, toggle restore, fullscreen exclusion', flush=True)
                else:
                    assert first['size'] == expected, (first['size'], expected)
                    assert first['at'] == expected_pos, (first,expected_pos)
                    print('PASS: saved size restored after compositor and application restart', flush=True)
                if attempt == 1:
                    # A global false ignores stored positions; a rule can override it.
                    ok('eval hl.plugin.float_memory.configure({remember_position=false,center=true})')
                    toggle(); toggle()
                    centered = windows()[0]['at']
                    assert centered != expected_pos, (centered, expected_pos)
                    ok('eval hl.plugin.float_memory.configure({remember_position=false,rule=function(w) return {remember_position=true} end})')
                    toggle(); toggle()
                    assert windows()[0]['at'] == expected_pos, windows()[0]
                    ok('eval assert(not pcall(hl.plugin.float_memory.configure,{remember_position="bad"}))')
                    # Programmatic changes must not change the persisted preferred position.
                    before_program = geometry(state_file.read_bytes())
                    ok('eval hl.dispatch(hl.dsp.window.move({x=10,y=10,relative=true}))')
                    time.sleep(.3)
                    assert geometry(state_file.read_bytes()) == before_program
                    toggle(); toggle()
                    assert windows()[0]['at'] == expected_pos
                    base_config = config.read_text()
                    setup = 'hl.plugin.float_memory.configure({remember_position=false,default_size={width=700,height=500},rule=function(w) assert(w.class == "hfm-integration-test"); return {key="test/custom",role="preview",default_size={width=640,height=480},center=false} end})'
                    config.write_text(base_config + '\nif hl.plugin.float_memory and hl.plugin.float_memory.configure then ' + setup + ' end\n')
                    ok('reload')
                    time.sleep(.3)
                    toggle(); toggle()
                    assert windows()[0]['size'] == [640, 480], windows()[0]
                    ok('plugin load ' + str(driver))
                    time.sleep(.5)
                    custom_size = windows()[0]['size']
                    assert custom_size != [640, 480]
                    custom_before = geometry(state_file.read_bytes())
                    assert next(row for row in custom_before if row[0] == 'test/custom')[4] is None
                    ok('eval hl.plugin.float_memory_test.drag(true,false,20,10)')
                    time.sleep(.5)
                    assert geometry(state_file.read_bytes()) == custom_before, 'disabled move was persisted' 
                    ok('plugin unload ' + str(driver))
                    state_file = state / 'hypr-float-memory/sizes.cbor'
                    wait_for(lambda: b'test/custom' in state_file.read_bytes())
                    before = state_file.read_bytes()
                    ok('eval assert(not pcall(hl.plugin.float_memory.configure,{default_size={width=0,height=20}}))')
                    assert b'test/custom' in before and b'preview' in before
                    config.write_text(base_config + '\nif hl.plugin.float_memory and hl.plugin.float_memory.configure then hl.plugin.float_memory.configure({rule=function(w) return false end}) end\n')
                    ok('reload')
                    time.sleep(.3)
                    toggle(); toggle()
                    ok('plugin load ' + str(driver))
                    time.sleep(.5)
                    ok('plugin unload ' + str(driver))
                    assert geometry(state_file.read_bytes()) == geometry(before), 'excluded window was persisted'
                    config.write_text(base_config + '\nif hl.plugin.float_memory and hl.plugin.float_memory.configure then ' + setup + ' end\n')
                    ok('reload')
                    time.sleep(.3)
                    toggle(); toggle()
                    assert windows()[0]['size'] == custom_size, (windows()[0], custom_size)
                    config.write_text(base_config)
                    ok('reload')
                    time.sleep(.3)
                    toggle(); toggle()
                    assert windows()[0]['size'] == expected, (windows()[0], expected)
                    ok('eval hl.plugin.float_memory.configure({rule=function(w) error("intentional test error") end})')
                    toggle()
                    error_result = command('eval hl.dispatch(hl.dsp.window.float({action="toggle"}))')
                    assert 'intentional test error' in error_result, error_result
                    time.sleep(.2)
                    assert geometry(state_file.read_bytes()) == geometry(before)
                    ok('eval hl.plugin.float_memory.configure({})')
                    toggle(); toggle()
                    assert windows()[0]['size'] == expected
                    print('PASS: Lua policies, custom keys/sizes, exclusion, invalid config, reload, callback errors', flush=True)
                ok('plugin unload ' + str(plugin))
                assert json.loads(command('j/plugin list')) == []
                print('PASS: retention on load and plugin unload', flush=True)
            except Exception:
                print((root / f'compositor-{attempt}.log').read_text()[-6000:])
                raise
            finally:
                if client:
                    client.terminate()
                    try: client.wait(timeout=3)
                    except subprocess.TimeoutExpired: client.kill(); client.wait()
                compositor.terminate()
                try: compositor.wait(timeout=5)
                except subprocess.TimeoutExpired: compositor.kill(); compositor.wait()
