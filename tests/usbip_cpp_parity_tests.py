# SPDX-License-Identifier: GPL-2.0-or-later
"""Same controlled RF/URB scenarios against production Python and C++ cores."""
import asyncio
import json
from pathlib import Path
import struct
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'src'))
from usb_gip_radio import RadioBackend
from usbip_urb import Submit,IsoPacket


class Writer:
    def __init__(self,backend,status):self.backend,self.status,self.tx=backend,status,[]
    def write(self,data):
        self.tx.append(data)
        token=struct.unpack_from('<I',data,8)[0]
        if self.status is not None:self.backend.pending[token].set_result(self.status)
    async def drain(self):pass


def request(seq,count=8):
    block=bytes((0x60,32,1,0xc0,0x81,0))+bytes((77,))*192
    return Submit(seq,0,2,0,len(block)*count,42,count,1,bytes(8),block*count,
                  tuple(IsoPacket(i*len(block),len(block)) for i in range(count)))


async def scenario(mode):
    now=[100.0]
    backend=RadioBackend('020000000001',audio_bridge=True,clock=lambda:now[0])
    backend.wcid=1;backend.generation=1;backend.audio_layout=True
    writer=Writer(backend,None if mode=='ack_timeout' else -18 if mode=='expired_ack' else -5 if mode in ('error_ack','recover_ack') else 0)
    backend.writer=writer
    if mode in ('error_ack','ok_ack','recover_ack','ack_timeout','expired_ack'):
        backend.render_credits=1;backend.render_credit_time=now[0]
        if mode=='ack_timeout':
            original=asyncio.wait_for
            async def timeout_ack(awaitable,timeout):
                if isinstance(awaitable,asyncio.Future) and timeout==5:raise TimeoutError()
                return await original(awaitable,timeout)
            with patch('usb_gip_radio.asyncio.wait_for',timeout_ack):
                result=await backend.render_audio(request(10))
        else:result=await backend.render_audio(request(10))
        if mode=='recover_ack':
            assert backend.recovery_required and not backend.ready and backend.wcid==1
            backend.imported();now[0]+=.001
            backend.audio_layout=True;backend.render_credits=1;backend.render_credit_time=now[0]
            writer.status=0
            result=await backend.render_audio(request(20))
        state=dict(ready=backend.ready,generation=backend.generation,wcid=backend.wcid,
                   recovery=backend.recovery_required,status=result.status)
        if mode=='expired_ack':state.update(expired=backend.render_dropped,iso_status=result.iso[0].status)
        return state
    if mode=='pcm_age':
        result=await backend.render_audio(request(11,4))
        assert result.status==0 and not writer.tx
        now[0]=100.060
        pending=asyncio.create_task(backend.render_audio(request(12,4)))
    else:
        now[0]=100.040
        pending=asyncio.create_task(backend.render_audio(request(11)))
    async def staged():
        while len(backend.render_pcm)!=1536:await asyncio.sleep(0)
    await asyncio.wait_for(staged(),1)
    for _ in range(4):await asyncio.sleep(0)
    now[0]=100.070 if mode=='pcm_age' else 100.105
    if mode=='pcm_age':
        backend.render_credits=1;backend.render_credit_time=now[0]
        async with backend.changed:backend.changed.notify_all()
    result=await asyncio.wait_for(pending,1)
    if mode=='pcm_age':
        return dict(tx=len(writer.tx),expired=backend.render_dropped,oldest_pcm_age_ms=70,
                    status=result.status,iso_status=result.iso[0].status,actual=result.actual)
    return dict(status=result.status,iso_status=result.iso[0].status,frame=result.frame,tx=len(writer.tx))


class DifferentialTests(unittest.IsolatedAsyncioTestCase):
    async def compare(self,mode):
        binary=ROOT/'outputs/usbip-parity-probe.exe'
        if not binary.exists():self.skipTest('Run scripts/Test-CppBridge.cmd first')
        expected=await scenario(mode)
        native=await asyncio.to_thread(subprocess.check_output,[str(binary),mode],text=True,timeout=5)
        actual=json.loads(native)
        self.assertEqual(actual,expected,mode)
        return actual

    async def test_successful_audio_ack(self):
        self.assertTrue((await self.compare('ok_ack'))['ready'])

    async def test_audio_error_invalidates_usb_without_losing_rf(self):
        state=await self.compare('error_ack')
        self.assertFalse(state['ready']);self.assertTrue(state['recovery']);self.assertEqual(state['wcid'],1)

    async def test_audio_ack_timeout_uses_same_recovery(self):
        state=await self.compare('ack_timeout')
        self.assertFalse(state['ready']);self.assertEqual(state['status'],-19)

    async def test_unsent_media_expiry_does_not_reset_usb_or_rf(self):
        state=await self.compare('expired_ack')
        self.assertTrue(state['ready']);self.assertFalse(state['recovery'])
        self.assertEqual((state['generation'],state['expired'],state['iso_status']),(1,1,-18))

    async def test_usb_reimport_recovers_without_rf_reassociation(self):
        state=await self.compare('recover_ack')
        self.assertTrue(state['ready']);self.assertFalse(state['recovery']);self.assertEqual(state['generation'],2)

    async def test_expiry_reports_per_iso_error_and_original_frame(self):
        state=await self.compare('expiry')
        self.assertEqual((state['status'],state['iso_status'],state['frame']),(0,-18,42))

    async def test_old_partial_pcm_is_not_acknowledged_as_sent(self):
        state=await self.compare('pcm_age')
        self.assertEqual((state['tx'],state['expired'],state['actual'],state['iso_status']),(0,1,0,-18))


if __name__=='__main__':unittest.main()
