# SPDX-License-Identifier: GPL-2.0-or-later
"""On-demand isolated USBIP workers; radio/audio never flows through this process."""
import argparse
import asyncio
from collections import deque
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import json
import multiprocessing
from pathlib import Path
import re
import socket
import subprocess
import sys
import time
import uuid
from bridge_telemetry import BridgeTelemetry
import development_trace as trace
from usbip_auto_worker import PROFILES, model, serial_for, worker_main


def read_json(path):
    try:
        return json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError,ValueError):
        return None


def read_error_tail(path):
    try:
        with path.open('rb') as stream:
            stream.seek(0, 2)
            stream.seek(max(0, stream.tell()-4096))
            return stream.read().decode('utf-8', errors='replace').splitlines()[-8:]
    except OSError as error:
        return [type(error).__name__]


def process_identity(pid):
    kernel = ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = (wintypes.DWORD,wintypes.BOOL,wintypes.DWORD)
    handle = kernel.OpenProcess(0x1000,False,pid)
    if not handle:
        raise OSError(ctypes.get_last_error(),'Cannot identify owned process')
    created,ended,kernel_time,user_time = (wintypes.FILETIME() for _ in range(4))
    kernel.GetProcessTimes.argtypes = (wintypes.HANDLE,)+(ctypes.POINTER(wintypes.FILETIME),)*4
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    try:
        if not kernel.GetProcessTimes(handle,ctypes.byref(created),ctypes.byref(ended),ctypes.byref(kernel_time),ctypes.byref(user_time)):
            raise OSError(ctypes.get_last_error(),'Cannot read process creation time')
        return {'Pid':pid,'StartTicks':((created.dwHighDateTime<<32)|created.dwLowDateTime)+504911232000000000}
    finally:
        kernel.CloseHandle(handle)


class Registry:
    def __init__(self,path):
        self.path = path
        self.devices = {}
        if path.exists():
            document = read_json(path)
            if not isinstance(document,dict) or document.get('Version')!=1 or not isinstance(document.get('Devices'),list):
                raise ValueError('Invalid automatic pairing registry; existing file preserved')
            if len(document['Devices'])>256:
                raise ValueError('Automatic registry exceeds 256 remembered devices')
            for item in document['Devices']:
                mac,pid = item['Mac'],item['Pid']
                if mac in self.devices or item['VirtualSerial']!=serial_for(mac,pid) or model(0x045e,item['RadioPid'])!=pid:
                    raise ValueError('Invalid or repeated saved USB identity')
                self.devices[mac] = item

    def remember(self,info):
        mac,pid = info['Mac'],info['Pid']
        if info['VirtualSerial']!=serial_for(mac,pid) or model(0x045e,info['RadioPid'])!=pid:
            raise ValueError('Worker identity does not match a supported profile')
        if mac not in self.devices and len(self.devices)>=256:
            return False
        self.devices[mac] = {'Mac':mac,'Pid':pid,'RadioPid':info['RadioPid'],
                             'Profile':PROFILES[pid],'VirtualSerial':info['VirtualSerial'],
                             'LastSeenUtc':datetime.now(timezone.utc).isoformat()}
        return True

    def document(self):
        return {'Version':1,'Devices':[self.devices[m] for m in sorted(self.devices)]}


class Slot:
    def __init__(self,index,base,work):
        self.index,self.usb_port,self.radio_port = index,base+2*index,base+2*index+1
        self.status = work/f'slot-{index}-status.json'
        self.process = self.identity = self.pending = None
        self.run_id = None
        self.port = 0
        self.imported_generation = None
        self.info = {}
        self.task = None
        self.retry_at = self.attached_at = 0
        self.remembered = False
        self.log = self.error = None


class Supervisor:
    def __init__(self,root,adapter,usbip,base=33400,count=8,pair=False,radio_factory=subprocess.Popen,idle_channel_scan=False,preserve_old_wireless_mode=True):
        if not 1<=count<=16 or base<1024 or base+2*count>65535:
            raise ValueError('Invalid automatic slot pool')
        self.root,self.adapter,self.usbip = root,adapter,str(usbip)
        self.work = root/'work/auto-usb'
        self.work.mkdir(parents=True,exist_ok=True)
        (root/'config').mkdir(exist_ok=True)
        self.registry = Registry(root/'config/paired-devices.local.json')
        self.registry_io = BridgeTelemetry(self.registry.path,emit=lambda event:None)
        self.state_io = BridgeTelemetry(root/'work/usb-gip-running.json')
        self.slots = [Slot(i,base,self.work) for i in range(count)]
        self.base,self.pair = base,pair
        self.idle_channel_scan = idle_channel_scan
        self.preserve_old_wireless_mode = preserve_old_wireless_mode
        self.radio_factory = radio_factory
        self.radio_observed = {}
        self.me = process_identity(multiprocessing.current_process().pid)
        self.radio = self.radio_identity = None
        self.radio_status = self.work/'radio-status.json'
        self.radio_stop = root/'work/usb-gip.stop'
        stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
        self.radio_log,self.radio_error = self.work/f'radio-{stamp}.log',self.work/f'radio-{stamp}.err'
        self.listeners,self.accept_tasks = [],[]
        self.history = deque(maxlen=32)
        self.usbip_lock = asyncio.Lock()
        self.failure = None
        self.stopping = False

    async def usbip_command(self,*arguments):
        started = time.perf_counter_ns()
        trace.event('usbip_command_queued', arguments=arguments)
        async with self.usbip_lock:
            trace.event('usbip_command_started', arguments=arguments,
                        queue_wait_us=(time.perf_counter_ns()-started)/1000)
            if self.stopping and 'attach' in arguments:
                raise RuntimeError('Automatic bridge is stopping')
            result = await asyncio.to_thread(subprocess.run,[self.usbip,*arguments],capture_output=True,
                                             timeout=20,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        trace.event('usbip_command_finished', suspicious=result.returncode != 0,
                    arguments=arguments, returncode=result.returncode,
                    elapsed_us=(time.perf_counter_ns()-started)/1000,
                    stdout_bytes=len(result.stdout), stderr_bytes=len(result.stderr))
        if result.returncode:
            raise RuntimeError('USBIP '+arguments[0]+': '+result.stderr.decode(errors='replace').strip()[:240])
        return result.stdout.decode(errors='replace').strip()

    async def detach(self,slot):
        if slot.port:
            try:
                await self.usbip_command('detach','-p',str(slot.port))
            except RuntimeError:
                # VHCI may already have removed the port when its TCP peer exited.
                # Verify absence rather than matching a localized Windows error.
                listing = await self.usbip_command('port')
                active = {int(port) for port in re.findall(r'^Port\s+(\d+):',listing,re.MULTILINE)}
                if listing.strip() and not active:
                    raise RuntimeError('Cannot verify USBIP ports after failed detach')
                if slot.port in active:
                    raise
            slot.port = 0
            slot.imported_generation = None

    async def accept(self,slot,listener):
        while True:
            accepted,_ = await asyncio.get_running_loop().sock_accept(listener)
            accepted.setblocking(False)
            if slot.pending is not None:
                accepted.close()  # One bounded pending association per slot.
            else:
                slot.pending = accepted

    async def start_worker(self,slot):
        sock,slot.pending = slot.pending,None
        slot.run_id = uuid.uuid4().hex
        slot.log = self.work/f'slot-{slot.index}-{slot.run_id}.log'
        slot.error = self.work/f'slot-{slot.index}-{slot.run_id}.err'
        slot.info,slot.remembered,slot.retry_at = {},False,0
        context = multiprocessing.get_context('spawn')
        process = context.Process(target=worker_main,args=(sock,str(self.root),slot.usb_port,
                                  str(slot.status),slot.run_id,str(slot.log),str(slot.error),self.preserve_old_wireless_mode),daemon=True)
        try:
            await asyncio.to_thread(process.start)
            slot.process = process
            slot.identity = process_identity(process.pid)
        finally:
            sock.close()  # The child owns a duplicated kernel socket handle.

    async def maintain(self,slot):
        if slot.process is not None:
            if slot.pending is not None and slot.process.is_alive():
                # C++ opens a fresh socket only after releasing the old radio
                # association. A stalled old Windows import cannot own this
                # slot forever or block the next model that uses its slot.
                slot.info = dict(slot.info,Ready=False)
                await self.detach(slot)
                slot.process.terminate()
                await asyncio.to_thread(slot.process.join,2)
                if slot.process.is_alive():
                    raise RuntimeError('Owned previous USBIP worker did not exit')
            info = await asyncio.to_thread(read_json,slot.status)
            if info is not None and info.get('RunId')==slot.run_id:
                slot.info = info
            if not slot.process.is_alive():
                await self.detach(slot)
                slot.process.join(0)
                if slot.info.get('RejectedReason'):
                    self.history.append(dict(Mac=slot.info.get('Mac'),Reason=slot.info['RejectedReason']))
                slot.process.close()
                slot.process = slot.identity = None
                slot.info = {}
            elif slot.info.get('Ready') or (slot.info.get('RecoveryRequired') and slot.info.get('RadioAssociated')):
                if not slot.remembered:
                    if self.registry.remember(slot.info):
                        self.registry_io.publish(self.registry.document())
                    slot.remembered = True
                now = time.monotonic()
                if slot.port and ((slot.imported_generation is not None and slot.info.get('Generation')!=slot.imported_generation) or
                                  ((slot.info.get('RecoveryRequired') or not slot.info.get('UsbImported')) and now-slot.attached_at>2)):
                    await self.detach(slot)
                    slot.retry_at = now+1
                if not slot.port and now>=slot.retry_at:
                    slot.retry_at = now+2
                    try:
                        text = await self.usbip_command('-t',str(slot.usb_port),'attach','-r','127.0.0.1','-b','1-1','--once','--terse')
                        if not re.fullmatch(r'\d{1,3}',text) or not 1<=int(text)<=255:
                            raise RuntimeError('Cannot identify this worker\'s USBIP port')
                        slot.port,slot.attached_at = int(text),time.monotonic()
                        slot.imported_generation = slot.info.get('Generation')
                    except (RuntimeError,subprocess.TimeoutExpired) as error:
                        self.state_io.event({'operation':'attach_retry','slot':slot.index,'detail':str(error)})
            elif slot.port:
                await self.detach(slot)
        if slot.process is None and slot.pending is not None:
            await self.start_worker(slot)

    def state(self,running):
        devices = []
        for slot in self.slots:
            if slot.process is not None:
                info = slot.info
                devices.append(dict(Name='slot-'+str(slot.index),Mac=info.get('Mac'),Pid=info.get('Pid'),
                    Connector=info.get('Connector'),
                    Audio=info.get('Pid') in (0x0b17,0x0b32),UsbPort=slot.usb_port,RadioPort=slot.radio_port,
                    Profile=str(self.root/'driver/profiles'/info['Profile']) if info.get('Profile') else None,
                    VirtualSerial=info.get('VirtualSerial'),Status=str(slot.status),Server=slot.identity,
                    ServerLog=str(slot.log),ServerError=str(slot.error),Port=slot.port,
                    Generation=info.get('Generation',0),Ready=info.get('Ready',False),
                    RejectedReason=info.get('RejectedReason')))
        return dict(Running=running,BridgeBackend='python-auto',Automatic=True,DurationSeconds=0,Unlimited=True,
                    FallbackOutput='none',NativeController=True,NewHeadsetOnly=False,OldHeadsetIndependentClock=True,
                    Worker=self.me,Radio=self.radio_identity,ControllerStatus=str(self.radio_status),
                    RadioLog=str(self.radio_log),RadioError=str(self.radio_error),Devices=devices,
                    SlotCapacity=len(self.slots),RememberedDevices=len(self.registry.devices),
                    IdleChannelScan=self.idle_channel_scan,
                    PreserveOldWirelessMode=self.preserve_old_wireless_mode,
                    Pairing=self.radio_observed.get('Pairing',False),RadioChannel=self.radio_observed.get('RadioChannel'),
                    PairingScanDeferred=bool(self.radio_observed.get('Pairing') and self.radio_observed.get('Controllers')),
                    RejectedDevices=list(self.history),Error=self.failure)

    async def run(self):
        log = error = None
        monitor = asyncio.create_task(trace.monitor_loop(), name='development-loop-monitor') if trace.active() else None
        try:
            for slot in self.slots:
                listener = socket.socket()
                listener.bind(('127.0.0.1',slot.radio_port))
                listener.listen(2)
                listener.setblocking(False)
                self.listeners.append(listener)
                self.accept_tasks.append(asyncio.create_task(self.accept(slot,listener)))
            self.radio_stop.unlink(missing_ok=True)
            log,error = self.radio_log.open('wb'),self.radio_error.open('wb')
            args = [str(self.root/'outputs/xone-probe.exe'),'run','--instance',self.adapter,
                    '--output','usb-gip','--usb-auto-slots',str(len(self.slots)),
                    '--usb-gip-port',str(self.base+1),'--seconds','0',
                    '--firmware',str(self.root/'work/firmware/FW_ACC_00U.bin'),
                    '--stop-file',str(self.radio_stop),'--status-file',str(self.radio_status)]
            if self.pair:
                args.append('--pair-beacon')
            if self.idle_channel_scan:
                args.append('--idle-channel-scan')
            self.radio = self.radio_factory(args,stdout=log,stderr=error,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            self.radio_identity = process_identity(self.radio.pid)
            trace.event('radio_process_started', identity=self.radio_identity,
                        stdout=str(self.radio_log), stderr=str(self.radio_error))
            while not await asyncio.to_thread(self.radio_stop.exists):
                observed = await asyncio.to_thread(read_json,self.radio_status)
                if observed is not None:
                    self.radio_observed = observed
                if self.radio.poll() is not None:
                    tail = await asyncio.to_thread(read_error_tail, self.radio_error)
                    trace.event('radio_process_exited', suspicious=True, returncode=self.radio.returncode,
                                stderr=str(self.radio_error), error_tail=tail)
                    raise RuntimeError(f'Radio bridge exited with code {self.radio.returncode}; see {self.radio_error}')
                for task in self.accept_tasks:
                    if task.done():
                        task.result()
                        raise RuntimeError('Automatic radio listener stopped')
                for slot in self.slots:
                    if slot.task is not None and slot.task.done():
                        slot.task.result()
                        slot.task = None
                    if slot.task is None:
                        slot.task = asyncio.create_task(self.maintain(slot))
                self.state_io.publish(self.state(True))
                await asyncio.sleep(.25)
        except Exception as exception:
            self.failure = str(exception)
            self.state_io.event({'operation':'automatic_stopped','detail':self.failure})
        finally:
            trace.event('supervisor_stopping', suspicious=self.failure is not None, failure=self.failure)
            self.stopping = True
            for task in self.accept_tasks:
                task.cancel()
            await asyncio.gather(*self.accept_tasks,return_exceptions=True)
            for listener in self.listeners:
                listener.close()
            if self.radio is not None and self.radio.poll() is None:
                await asyncio.to_thread(self.radio_stop.write_text,'stop',encoding='ascii')
                try:
                    await asyncio.to_thread(self.radio.wait,timeout=5)
                except subprocess.TimeoutExpired:
                    self.radio.terminate()
                    await asyncio.to_thread(self.radio.wait,timeout=5)
            for slot in self.slots:
                if slot.task is not None:
                    await asyncio.gather(slot.task,return_exceptions=True)
                if slot.pending is not None:
                    slot.pending.close()
                try:
                    await self.detach(slot)
                except Exception as exception:
                    self.failure = (self.failure or '')+' Cleanup: '+str(exception)
                if slot.process is not None:
                    await asyncio.to_thread(slot.process.join,2)
                    if slot.process.is_alive():
                        slot.process.terminate()
                        await asyncio.to_thread(slot.process.join,2)
                    slot.process.close()
                    slot.process = None
            if log is not None:
                log.close()
            if error is not None:
                error.close()
            self.state_io.publish(self.state(False))
            await asyncio.to_thread(self.registry_io.close)
            await asyncio.to_thread(self.state_io.close)
            if monitor is not None:
                monitor.cancel()
                await asyncio.gather(monitor, return_exceptions=True)
        return 1 if self.failure else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--instance',required=True)
    parser.add_argument('--usbip',type=Path,required=True)
    parser.add_argument('--slots',type=int,default=8)
    parser.add_argument('--base-port',type=int,default=33400)
    parser.add_argument('--pair',action='store_true')
    parser.add_argument('--idle-channel-scan',action='store_true',help='Diagnostic channel search only while no radio clients are connected')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--preserve-old-wireless-mode',action='store_true',default=True,help='Suppress known USB mode initialization only for headset 2021 (default)')
    mode.add_argument('--forward-old-usb-mode',action='store_false',dest='preserve_old_wireless_mode',help='Diagnostic rollback to unchanged USB mode forwarding')
    args = parser.parse_args()
    if sys.platform!='win32':
        parser.error('The hardware supervisor requires Windows')
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    supervisor = Supervisor(args.root.resolve(),args.instance,args.usbip,args.base_port,args.slots,args.pair,idle_channel_scan=args.idle_channel_scan,preserve_old_wireless_mode=args.preserve_old_wireless_mode)
    return asyncio.run(supervisor.run())


if __name__=='__main__':
    multiprocessing.freeze_support()
    try:
        trace.start_from_environment('supervisor')
        sys.exit(main())
    finally:
        trace.stop()
