#!/usr/bin/env python3
"""Lifecycle/ownership regression for the Linux test-instance launcher, no assets."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ogmp import instance as engine

SERVER = r'''#!/usr/bin/env python3
import argparse,json,os,socket,sys
p=argparse.ArgumentParser();p.add_argument('--test-bridge',type=int);args,extra=p.parse_known_args()
with socket.socket()as listener:
 listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);listener.bind(('127.0.0.1',args.test_bridge));listener.listen()
 connection,_=listener.accept()
 with connection,connection.makefile('r',encoding='utf-8')as incoming:
  for line in incoming:
   request=json.loads(line);cmd=request['cmd']
   if cmd=='screenshot':
    with open(request['path'],'wb')as image:image.write(b'fixture screenshot')
   response={'id':request['id'],'ok':True,'pid':os.getpid(),'argv':sys.argv,'preload':os.environ.get('LD_PRELOAD','')}
   connection.sendall((json.dumps(response)+'\n').encode())
   if cmd=='quit':break
'''

def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class OwnershipTests(unittest.TestCase):
    def test_profile_requires_one_absolute_owned_write_directory(self):
        with tempfile.TemporaryDirectory() as root, patch.object(engine, 'RUN_DIR', root):
            owned = os.path.join(root, 'player', 'writedir')
            self.assertTrue(engine._owns_profile(['game', '--write-dir', owned]))
            self.assertTrue(engine._owns_profile(['game', '--write-dir=' + owned]))
            for argv in (['game', '--log', owned], ['game', '--write-dir', root],
                         ['game', '--write-dir', root + '-unrelated/writedir'],
                         ['game', '--write-dir', 'relative/writedir'],
                         ['game', '--write-dir', owned, '--write-dir', '/outside']):
                self.assertFalse(engine._owns_profile(argv), argv)

    def test_instance_cannot_erase_parent_or_escape_through_symlink(self):
        with tempfile.TemporaryDirectory() as root, patch.object(engine, 'RUN_DIR', root):
            for name in ('..', '.', os.path.dirname(root)):
                with self.assertRaises(ValueError):
                    engine.Instance(name)
            if os.name != 'nt':
                os.symlink(os.path.dirname(root), os.path.join(root, 'escape'))
                with self.assertRaises(ValueError):
                    engine.Instance('escape/profile')


@unittest.skipUnless(sys.platform.startswith('linux'), 'Linux /proc lifecycle')
class LinuxLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ogmp instance test ')
        self.root = Path(self.temp.name)
        self.program = self.root / 'fake engine.py'
        self.program.write_text(SERVER)
        self.program.chmod(0o755)
        self.environment = patch.dict(os.environ, {'OGMP_EXE': str(self.program), 'OGMP_WORKDIR': str(self.root),
                                                  'OGMP_EXTRA': '--game-data "some Steam folder"'})
        self.environment.start()
        self.run_dir = patch.object(engine, 'RUN_DIR', str(self.root / 'owned runs'))
        self.run_dir.start()
        self.port = patch.object(engine, 'BASE_PORT', free_port())
        self.port.start()
        self.instances = []
        self.outsiders = []

    def tearDown(self):
        for inst in self.instances:
            inst.stop(graceful=False)
        for process in self.outsiders:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
        self.port.stop()
        self.run_dir.stop()
        self.environment.stop()
        self.temp.cleanup()

    def start(self, name='one player'):
        inst = engine.Instance(name, level='Data/Levels/space level.xml', extra_args='--probe "two words"')
        self.instances.append(inst)
        return inst.start(bridge_timeout=5)

    def test_real_launch_argument_quoting_capture_and_graceful_cleanup(self):
        inst = self.start()
        state = inst.bridge.status()
        self.assertEqual(state['pid'], inst.pid)
        self.assertIn('two words', state['argv'])
        self.assertIn('some Steam folder', state['argv'])
        self.assertIn('Data/Levels/space level.xml', state['argv'])
        self.assertIn('/proc/self/fd/', state['preload'])
        with open(f'/proc/{inst.pid}/maps') as mappings:
            self.assertIn(str(Path(inst.dir) / 'linux_window_isolation.so'), mappings.read())
        self.assertIn(inst.pid, dict(engine.our_processes()))
        self.assertTrue(inst.alive())
        self.assertTrue(inst.capture(str(self.root / 'capture.png')))
        with self.assertRaises(RuntimeError):
            engine.Instance('one player').prepare()
        inst.stop()
        self.assertIsNone(inst.pid)
        self.assertEqual(inst.process.returncode, 0)
        self.assertFalse(engine.our_processes())
        self.assertTrue((Path(inst.dir) / 'console.log').exists())

    def test_stop_all_and_hard_kill_preserve_unowned_same_program(self):
        outside = subprocess.Popen([str(self.program), '--test-bridge', str(free_port()),
                                    '--write-dir', str(self.root / 'not owned')],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.outsiders.append(outside)
        time.sleep(.1)
        inst = self.start()
        self.assertTrue(engine.user_game_running())
        self.assertFalse(engine._kill_owned_pid(outside.pid))
        self.assertIsNone(outside.poll())
        pid = inst.pid
        self.assertEqual(engine.stop_all(), [pid])
        inst.process.wait(timeout=5)
        self.assertIsNone(outside.poll())
        inst.stop(graceful=False)
        again = self.start('second player')
        again.kill()
        self.assertIsNotNone(again.process.poll())
        self.assertIsNone(outside.poll())


if __name__ == '__main__':
    unittest.main()
