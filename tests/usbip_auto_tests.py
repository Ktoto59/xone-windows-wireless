# SPDX-License-Identifier: GPL-2.0-or-later
import asyncio
import json
import multiprocessing
import os
from pathlib import Path
import socket
import shutil
import struct
import sys
import tempfile
import time
import unittest
import uuid
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'src'))
from usbip_auto_worker import model, serial_for, worker_main
from usbip_auto_manager import Registry, Supervisor, Slot, read_json, process_identity
from usb_gip_radio import frame


class IdentityTests(unittest.TestCase):
    def test_only_observed_radio_models_select_captured_profiles(self):
        for radio,usb in ((0x0b31,0x0b17),(0x0b32,0x0b32),(0x0b13,0x0b12),(0x0b17,0x0b17)):
            self.assertEqual(model(0x045e,radio),usb)
        for vendor,pid in ((0x1234,0x0b32),(0x045e,0x9999)):
            with self.assertRaises(ValueError):model(vendor,pid)

    def test_same_model_instances_have_stable_distinct_serials_and_persist_without_manual_mac(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'paired.json'
            registry=Registry(path)
            self.assertEqual(registry.devices,{})
            for mac in ('020000000001','020000000002'):
                info=dict(Mac=mac,Pid=0x0b17,RadioPid=0x0b31,VirtualSerial=serial_for(mac,0x0b17))
                self.assertTrue(registry.remember(info))
            path.write_text(json.dumps(registry.document()),encoding='utf8')
            restored=Registry(path)
            self.assertEqual(len(restored.devices),2)
            self.assertNotEqual(*[v['VirtualSerial'] for v in restored.devices.values()])
            original=path.read_bytes()
            path.write_text('{bad-json',encoding='utf8')
            with self.assertRaises(ValueError):Registry(path)
            self.assertEqual(path.read_text(),'{bad-json')
            self.assertTrue(original)


class SpawnedWorkerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.work=Path(self.temp.name)
        self.nodes=[]
        self.usb_writers=[]

    async def asyncTearDown(self):
        for writer in self.usb_writers:
            writer.close()
        for n in self.nodes:
            n['writer'].close()
        for n in self.nodes:
            await asyncio.to_thread(n['process'].join,5)
            if n['process'].is_alive():
                n['process'].terminate()
                await asyncio.to_thread(n['process'].join,3)
            n['process'].close()
        self.temp.cleanup()

    async def start(self,mac,radio_pid,wcid=1,preserve=None):
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.setblocking(False)
        reader,writer=await asyncio.open_connection(*listener.getsockname())
        accepted,_=await asyncio.get_running_loop().sock_accept(listener)
        listener.close()
        reserve=socket.socket();reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1];reserve.close()
        run_id=uuid.uuid4().hex;status=self.work/(run_id+'.json')
        arguments=(accepted,str(ROOT),port,str(status),run_id,str(self.work/(run_id+'.log')),str(self.work/(run_id+'.err')))
        if preserve is not None:arguments+=(preserve,)
        process=multiprocessing.get_context('spawn').Process(target=worker_main,args=arguments)
        process.start();accepted.close()
        node=dict(process=process,reader=reader,writer=writer,status=status,port=port)
        self.nodes.append(node)
        body=bytearray(28);struct.pack_into('<HH',body,8,0x045e,radio_pid)
        announce=bytes((2,32,1,28))+body
        writer.write(frame(0,0,bytes.fromhex(mac))+frame(1,wcid,bytes.fromhex(mac))+frame(2,wcid,announce))
        await writer.drain()
        return node

    async def until_status(self,node,predicate):
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            value=read_json(node['status'])
            if value is not None and predicate(value):return value
            if not node['process'].is_alive() and value is None:
                self.fail('Spawned worker exited without status')
            await asyncio.sleep(.03)
        self.fail('Worker status timeout: '+str(read_json(node['status'])))

    async def usb_serial(self,node):
        reader,writer=await asyncio.open_connection('127.0.0.1',node['port'])
        self.usb_writers.append(writer)
        writer.write(struct.pack('!HHI',0x0111,0x8003,0)+b'1-1'.ljust(32,b'\0'));await writer.drain()
        self.assertEqual(await asyncio.wait_for(reader.readexactly(8),3),struct.pack('!HHI',0x0111,3,0))
        await reader.readexactly(312)
        async def descriptor(seq,value,index,length):
            setup=struct.pack('<BBHHH',0x80,6,value,index,length)
            writer.write(struct.pack('!IIIIIIiiii8s',1,seq,0x10001,1,0,0,length,-1,0,1,setup));await writer.drain()
            header=await asyncio.wait_for(reader.readexactly(48),3)
            self.assertEqual(struct.unpack_from('!i',header,20)[0],0)
            return await reader.readexactly(struct.unpack_from('!i',header,24)[0])
        device=await descriptor(1,0x0100,0,18)
        data=await descriptor(2,0x0300|device[16],0x0409,255)
        self.assertGreater(len(data),2)
        return data[2:].decode('utf-16le')

    async def test_two_same_model_workers_export_distinct_serials_and_one_disconnect_is_local(self):
        a=await self.start('020000000001',0x0b31,1)
        b=await self.start('020000000002',0x0b31,2)
        sa,sb=await asyncio.gather(self.until_status(a,lambda s:s.get('Ready')),self.until_status(b,lambda s:s.get('Ready')))
        self.assertEqual((sa['Pid'],sb['Pid']),(0x0b17,0x0b17))
        self.assertTrue(sa['IndependentAudioClock'] and sb['IndependentAudioClock'])
        self.assertTrue(sa['PreserveWirelessMode'] and sb['PreserveWirelessMode'])
        serial_a,serial_b=await asyncio.gather(self.usb_serial(a),self.usb_serial(b))
        self.assertNotEqual(serial_a,serial_b)
        self.assertEqual(serial_a,sa['VirtualSerial'])
        a['writer'].close();await a['writer'].wait_closed()
        await self.until_status(a,lambda s:not s.get('Ready'))
        await asyncio.to_thread(a['process'].join,3)
        self.assertFalse(a['process'].is_alive(),'RF EOF left a Windows USB import holding the worker alive')
        self.assertTrue(b['process'].is_alive())
        self.assertTrue((await self.until_status(b,lambda s:s.get('Ready')))['Ready'])

    async def test_unknown_model_never_becomes_a_windows_device(self):
        node=await self.start('020000000003',0x9999)
        status=await self.until_status(node,lambda s:s.get('RejectedReason'))
        self.assertFalse(status['Ready'])
        self.assertIsNone(status['Pid'])
        self.assertIn('Unsupported radio model',status['RejectedReason'])

    async def test_development_trace_survives_spawn_and_records_usb_and_rf_disconnect(self):
        logs=self.work/'development'
        with patch.dict(os.environ, {'XONE_DEVELOPMENT_LOG_DIR':str(logs), 'XONE_DEVELOPMENT_TRACE':'calls'}):
            node=await self.start('020000000009',0x0b32)
        await self.until_status(node,lambda s:s.get('Ready'))
        await self.usb_serial(node)
        node['writer'].close()
        await node['writer'].wait_closed()
        await asyncio.to_thread(node['process'].join,8)
        self.assertFalse(node['process'].is_alive())
        rows=[json.loads(line) for path in logs.glob('worker-*.jsonl')
              for line in path.read_text(encoding='utf-8').splitlines()]
        events=[r['fields']['event']['operation'] for r in rows if r['operation']=='bridge_event']
        self.assertIn('submit',events)
        self.assertIn('complete',events)
        self.assertTrue(any(r['operation']=='python_exception' for r in rows))
        anomalies=[json.loads(line) for path in logs.glob('anomalies-*.jsonl')
                   for line in path.read_text(encoding='utf-8').splitlines()]
        self.assertTrue(any(r['operation']=='bridge_event' and
                            r['fields']['event']['operation']=='radio_closed' for r in anomalies))

    async def test_experimental_usb_mode_adaptation_is_scoped_to_old_headset(self):
        for n,pid in enumerate((0x0b17,0x0b32,0x0b12),1):
            node=await self.start(f'0200000000{n:02x}',pid,n,preserve=True)
            status=await self.until_status(node,lambda s:s.get('Ready'))
            self.assertEqual(status['PreserveWirelessMode'],pid==0x0b17)
            self.assertEqual(status['IndependentAudioClock'],pid==0x0b17)
            self.assertEqual(status['RenderMaxAgeMs'],96 if pid==0x0b17 else 64)
            self.assertEqual(status['Connector'],{0x0b17:'headset-2021',0x0b32:'headset-2024',0x0b12:'series-controller'}[pid])



class SupervisorTests(unittest.IsolatedAsyncioTestCase):
    async def test_next_association_replaces_stalled_worker_without_touching_sibling(self):
        with tempfile.TemporaryDirectory() as directory:
            supervisor=object.__new__(Supervisor)
            slot=Slot(0,44000,Path(directory));sibling=Slot(1,44000,Path(directory))
            class Stalled:
                def __init__(self):self.alive=True;self.closed=False
                def is_alive(self):return self.alive
                def terminate(self):self.alive=False
                def join(self,timeout):pass
                def close(self):self.closed=True
            previous=slot.process=Stalled();other=sibling.process=Stalled()
            slot.pending=object();slot.port=7;slot.info={'Ready':True}
            sibling.port=8;commands=[];started=[]
            async def usbip(*args):commands.append(args);return ''
            async def start(node):
                self.assertFalse(previous.alive)
                self.assertTrue(previous.closed)
                self.assertEqual(node.port,0)
                started.append(node);node.pending=None
            supervisor.usbip_command=usbip;supervisor.start_worker=start
            await supervisor.maintain(slot)
            self.assertEqual(started,[slot])
            self.assertEqual(commands,[('detach','-p','7')])
            self.assertTrue(other.alive)
            self.assertEqual(sibling.port,8)

    async def test_already_removed_port_does_not_stop_other_devices(self):
        # Exercise the observed race with VHCI's automatic removal on TCP EOF.
        supervisor=object.__new__(Supervisor)
        class Node:port=7
        slot=Node();commands=[]
        async def absent(*args):
            commands.append(args)
            if args[0]=='detach':raise RuntimeError('localized error')
            self.assertEqual(args,('port',))
            return 'Imported USB devices\n====================\nPort 08: device in use at high-speed\n'
        supervisor.usbip_command=absent
        await supervisor.detach(slot)
        self.assertEqual(slot.port,0)
        self.assertEqual(commands,[('detach','-p','7'),('port',)])
        slot.port=7
        async def present(*args):
            if args[0]=='detach':raise RuntimeError('real detach failure')
            return 'Imported USB devices\n====================\nPort 07: device in use at high-speed\n'
        supervisor.usbip_command=present
        with self.assertRaisesRegex(RuntimeError,'real detach failure'):
            await supervisor.detach(slot)
        self.assertEqual(slot.port,7)

    async def test_on_demand_worker_registry_reconnect_and_owned_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            shutil.copytree(ROOT/'driver/profiles',root/'driver/profiles')
            loop=asyncio.get_running_loop()
            class FakeRadio:
                pid=os.getpid()
                def __init__(self,*args,**kwargs):self.stopped=False;self.writers=[]
                def poll(self):return 0 if self.stopped else None
                def wait(self,timeout=5):
                    self.stopped=True
                    for w in self.writers:loop.call_soon_threadsafe(w.close)
                    return 0
                def terminate(self):self.wait()
            # Reserve both adjacent ports before releasing them to the fixture.
            for base in range(41000,45000,2):
                x,y=socket.socket(),socket.socket()
                try:x.bind(('127.0.0.1',base));y.bind(('127.0.0.1',base+1));break
                except OSError:pass
                finally:x.close();y.close()
            supervisor=Supervisor(root,'FAKE-ADAPTER','unused-usbip',base,1,radio_factory=FakeRadio)
            ports={};commands=[]
            async def fake_usbip(*args):
                commands.append(args)
                if 'attach' in args:
                    r,w=await asyncio.open_connection('127.0.0.1',int(args[1]))
                    w.write(struct.pack('!HHI',0x0111,0x8003,0)+b'1-1'.ljust(32,b'\0'));await w.drain()
                    self.assertEqual(await r.readexactly(8),struct.pack('!HHI',0x0111,3,0))
                    await r.readexactly(312)
                    self.assertNotIn(7,ports)
                    ports[7]=w
                    return '7'
                self.assertEqual(args,('detach','-p','7'))
                w=ports.pop(7);w.close();await w.wait_closed()
                return ''
            supervisor.usbip_command=fake_usbip
            run=asyncio.create_task(supervisor.run())
            async def until(predicate):
                deadline=time.monotonic()+10
                while not predicate():
                    if run.done():self.fail('Supervisor exited: '+str(supervisor.failure))
                    if time.monotonic()>deadline:self.fail('Supervisor lifecycle timeout')
                    await asyncio.sleep(.03)
            try:
                await until(lambda:supervisor.radio is not None)
                self.assertTrue(all(s.process is None for s in supervisor.slots))
                serials=[];pids=[]
                for _ in range(2):
                    r,w=await asyncio.open_connection('127.0.0.1',base+1)
                    supervisor.radio.writers.append(w)
                    mac=bytes.fromhex('020000000099');body=bytearray(28);struct.pack_into('<HH',body,8,0x045e,0x0b13)
                    w.write(frame(0,0,mac)+frame(1,1,mac)+frame(2,1,bytes((2,32,1,28))+body));await w.drain()
                    await until(lambda:supervisor.slots[0].port==7)
                    node=supervisor.slots[0]
                    serials.append(node.info['VirtualSerial']);pids.append(node.process.pid)
                    self.assertEqual(node.info['Pid'],0x0b12)
                    w.close();await w.wait_closed()
                    await until(lambda:node.process is None and node.port==0)
                self.assertEqual(serials[0],serials[1])
                self.assertNotEqual(pids[0],pids[1])
                self.assertEqual(len(supervisor.registry.devices),1)
                self.assertEqual(len([c for c in commands if c[0]=='detach']),2)
            finally:
                supervisor.radio_stop.write_text('stop',encoding='ascii')
                await asyncio.wait_for(run,10)
                for w in ports.values():w.close()
            self.assertIsNone(supervisor.failure)
            self.assertFalse(read_json(root/'work/usb-gip-running.json')['Running'])
            self.assertEqual(len(Registry(root/'config/paired-devices.local.json').devices),1)
if __name__=='__main__':
    if sys.platform=='win32':asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    unittest.main()
