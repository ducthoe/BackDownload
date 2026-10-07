#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BOOT = '11111111-2222-3333-4444-555555555555'
QUERY = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee'


def check(condition, message):
    if not condition:
        raise AssertionError(message)


with tempfile.TemporaryDirectory(prefix='backdownload-status-') as temporary:
    directory = Path(temporary)
    module = directory / 'module'
    module.mkdir()
    proc = directory / 'proc'
    (proc / 'sys/kernel/random').mkdir(parents=True)
    (proc / '1234').mkdir()
    (proc / '1234/comm').write_text('system_server\n')
    (proc / 'sys/kernel/random/boot_id').write_text(BOOT + '\n')
    (proc / 'sys/kernel/random/uuid').write_text(QUERY + '\n')
    (proc / 'uptime').write_text('200.00 400.00\n')
    policy = directory / 'policy'
    request = directory / 'request'
    request.write_text('')
    tools = directory / 'bin'
    tools.mkdir()
    shell = shutil.which('sh')
    (tools / 'id').write_text(f'#!{shell}\nprintf "0\\n"\n')
    (tools / 'id').chmod(0o755)
    config_log = directory / 'config'
    ksud = tools / 'ksud'
    ksud.write_text(f'#!{shell}\nprintf "%s\\n" "$@" > {shlex.quote(str(config_log))}\n')
    ksud.chmod(0o755)
    for source in (ROOT / 'module').iterdir():
        if source.is_file():
            text = source.read_text()
            text = text.replace('BD_STATUS_FILE=/data/system/backdownload.status',
                                'BD_STATUS_FILE=' + shlex.quote(str(policy)))
            text = text.replace('BD_REQUEST_FILE=/data/system/backdownload.request',
                                'BD_REQUEST_FILE=' + shlex.quote(str(request)))
            text = text.replace('BD_PROC=/proc', 'BD_PROC=' + shlex.quote(str(proc)))
            text = text.replace('/data/adb/ksu/bin/ksud', str(ksud))
            (module / source.name).write_text(text)
    baseline = (module / 'module.prop').read_text()
    env = {**os.environ, 'MODDIR': str(module), 'PATH': str(tools) + ':' + os.environ['PATH']}

    def execute(*arguments, timeout=20):
        return subprocess.run([shell, *arguments], env=env, text=True,
                              capture_output=True, timeout=timeout)

    def snapshot(flags=(1, 0, 1), state='valid', stamp=200, boot=BOOT, token='0', pid=1234):
        body = f'BD1 {boot} {pid} {token} {stamp} {state} {flags[0]} {flags[1]} {flags[2]}\n'.encode()
        policy.write_bytes(body.ljust(256, b'\0'))

    def description():
        result = execute('-c', '. "$MODDIR/status.sh"; bd_description; printf "%s" "$BD_DESCRIPTION"')
        check(result.returncode == 0, result.stderr)
        return result.stdout

    for lock in (0, 1):
        for maintenance in (0, 1):
            for at in (0, 1):
                snapshot((lock, maintenance, at))
                text = description()
                if at:
                    check('AT authorization enabled' in text, text)
                elif lock and not maintenance:
                    check('Download Mode blocked' in text, text)
                else:
                    check('AT patch inactive; Download Mode allowed' in text, text)

    for arguments in ({'stamp': 139}, {'stamp': 201}, {'boot': QUERY}, {'pid': 9999},
                      {'flags': (1, 0, 2)}, {'state': 'garbage'}, {'token': QUERY}):
        snapshot(**arguments)
        check('Policy not verified' in description(), f'Accepted invalid snapshot: {arguments}')
    policy.write_bytes(b'\x03')
    check('Policy not verified' in description(), 'Accepted old cached-success byte')
    snapshot((-1, -1, -1), state='read_failed')
    check('Policy read failed' in description(), 'Read failure retained success')
    snapshot((-1, -1, -1), state='unsupported')
    check('Unsupported DMC policy' in description(), 'Unsupported layout retained success')

    snapshot()
    check(execute(str(module / 'update-status.sh')).returncode == 0, 'Failed to publish description')
    check((module / 'module.prop').read_text() == baseline, 'Persisted KSU runtime success in module.prop')
    check('--temp\noverride.description\n' in config_log.read_text(), 'Missing temporary KSU override')
    snapshot(stamp=139)
    execute(str(module / 'update-status.sh'))
    check('Policy not verified' in config_log.read_text(), 'Stale snapshot retained success override')
    check(execute(str(module / 'update-status.sh'), '1').returncode != 0,
          'Legacy success argument can still manufacture success')

    # An authorized cached sample must not satisfy a new Action request. The
    # responder returns the changed, blocked policy only after seeing its nonce.
    snapshot()
    request.write_text('')
    def respond(flags):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if request.read_text().strip() == QUERY:
                time.sleep(0.15)
                snapshot(flags, token=QUERY)
                return
            time.sleep(0.01)
        raise AssertionError('Action did not request a fresh read')

    reader = threading.Thread(target=respond, args=((1, 0, 0),))
    reader.start()
    action = execute(str(module / 'action.sh'))
    reader.join()
    check(action.returncode == 0, action.stdout + action.stderr)
    check('Download Mode policy: BLOCKED' in action.stdout, 'Action displayed cached authorization')
    check('AT authorization: Disabled' in action.stdout, action.stdout)
    check('AT patch is inactive.' in action.stdout, action.stdout)
    check('Download Mode blocked' in config_log.read_text(), 'Action did not update description')

    (proc / 'sys/kernel/random/uuid').write_text(BOOT + '\n')
    request.write_text('')
    snapshot()
    no_response = execute(str(module / 'action.sh'), timeout=30)
    check(no_response.returncode != 0 and 'Current policy is unknown' in no_response.stdout,
          'Action fell back to cached success after timeout')
    check('No live policy response' in config_log.read_text(), 'Timeout retained success description')
    execute(str(module / 'update-status.sh'))
    check('Policy not verified' in config_log.read_text(), 'Monitor reused a pre-request snapshot')

    (module / 'disable').touch()
    disabled = execute(str(module / 'action.sh'))
    check(disabled.returncode != 0 and 'Module disabled' in disabled.stdout, disabled.stdout)
    (module / 'disable').unlink()
    (tools / 'id').write_text(f'#!{shell}\nprintf "10378\\n"\n')
    check('Root is required' in execute(str(module / 'action.sh')).stdout, 'Missing root error')
    (tools / 'id').write_text(f'#!{shell}\nprintf "0\\n"\n')

    # Managers without KSU get an explicitly labelled last observation.
    ksud.chmod(0o644)
    request.write_text('')
    snapshot()
    (module / '.status-description').unlink(missing_ok=True)
    execute(str(module / 'update-status.sh'))
    check('Last policy check: AT authorization enabled' in (module / 'module.prop').read_text(),
          'Missing fallback description')
    policy.unlink()
    execute(str(module / 'update-status.sh'))
    check('Policy not verified' in (module / 'module.prop').read_text(),
          'Unavailable policy retained fallback success')

    # Both boot scripts and the Zygisk companion can start the monitor. Only one
    # instance should remain, and disabling the module must stop that instance.
    service = module / 'service.sh'
    service.write_text(service.read_text().replace('sleep 5', 'sleep 0.1'))
    snapshot()
    running = subprocess.Popen([shell, str(service)], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        pid_file = module / '.status-service/pid'
        deadline = time.monotonic() + 3
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        check(pid_file.exists(), 'Status monitor did not start')
        check(execute(str(service), timeout=3).returncode == 0, 'Duplicate monitor did not exit')
        check(running.poll() is None, 'Original monitor stopped unexpectedly')
        (module / 'disable').touch()
        check(running.wait(timeout=3) == 0, 'Monitor did not stop when disabled')
        check(request.read_text().strip() == 'stop', 'Monitor did not stop the native reader')
        check(not pid_file.parent.exists(), 'Monitor retained its PID lock')
        check('Module inactive' in (module / 'module.prop').read_text(),
              'Disabled monitor retained authorized description')
    finally:
        if running.poll() is None:
            running.terminate()
            running.wait(timeout=3)

print('PASS: all policy states, boot/PID/freshness validation, live Action, timeout, root errors, monitor restart and disable')
