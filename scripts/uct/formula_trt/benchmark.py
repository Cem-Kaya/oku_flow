"""Native Windows TensorRT-only autoregressive formula runtime and timing."""
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import time
import numpy as np
assert platform.system()=='Windows', 'native Windows benchmark required'
# Explicit SDK roots belong to this developer process only.
BOOT=argparse.ArgumentParser(add_help=False)
BOOT.add_argument('--sdk',type=Path,required=True)
BOOT.add_argument('--cuda-bin',type=Path,required=True)
BOOT_ARGS,_=BOOT.parse_known_args()
SDK=BOOT_ARGS.sdk.resolve(); CUDA=BOOT_ARGS.cuda_bin.resolve()
os.environ['PATH']=os.pathsep.join((str(SDK/'bin'),str(CUDA),os.environ.get('PATH','')))
DLL_HANDLES=[os.add_dll_directory(str(p)) for p in (SDK/'bin',CUDA)]
import tensorrt as trt
from cuda.bindings import runtime as cu


def ck(result):
    if int(result[0])!=0: raise RuntimeError('CUDA error '+str(result[0]))
    return result[1] if len(result)==2 else result[1:]


class Buffer:
    def __init__(self,capacity,host=False):
        self.capacity=max(256,int(capacity)); self.host=host
        self.ptr=int(ck(cu.cudaHostAlloc(self.capacity,0)) if host else ck(cu.cudaMalloc(self.capacity)))
    def free(self):
        if self.ptr:
            ck(cu.cudaFreeHost(self.ptr) if self.host else cu.cudaFree(self.ptr)); self.ptr=0
    def host_view(self,dtype,shape):
        size=math.prod(shape)*np.dtype(dtype).itemsize
        assert self.host and size<=self.capacity
        return np.frombuffer((ctypes.c_ubyte*size).from_address(self.ptr),dtype=dtype).reshape(shape)


class OutputAllocator(trt.IOutputAllocator):
    def __init__(self,buffer):
        trt.IOutputAllocator.__init__(self); self.buffer=buffer; self.shape=None; self.refused=None
    def reallocate_output(self,name,memory,size,alignment):
        if size>self.buffer.capacity or self.buffer.ptr%alignment:
            self.refused=(name,size,alignment); return 0
        return self.buffer.ptr
    def reallocate_output_async(self,name,memory,size,alignment,stream):
        return self.reallocate_output(name,memory,size,alignment)
    def notify_shape(self,name,shape): self.shape=tuple(shape)


class FormulaRuntime:
    def __init__(self,directory):
        self.directory=Path(directory)
        from artifacts import verify_seal
        verify_seal(self.directory)
        self.meta=json.loads((self.directory/'partition.json').read_text(encoding='utf-8'))
        self.precision=json.loads((self.directory/'decoder.plan.json').read_text(encoding='utf-8'))['precision']
        self.log=trt.Logger(trt.Logger.WARNING)
        self.runtime=trt.Runtime(self.log)
        self.encoder=self.runtime.deserialize_cuda_engine((self.directory/'encoder.plan').read_bytes())
        self.decoder=self.runtime.deserialize_cuda_engine((self.directory/'decoder.plan').read_bytes())
        assert self.encoder and self.decoder, 'TensorRT engine loading failed; no fallback'
        self.enc=self.encoder.create_execution_context(); self.dec=self.decoder.create_execution_context()
        self.stream=ck(cu.cudaStreamCreate()); self.buffers=[]; self.healthy=True
        self.initial=dict(np.load(self.directory/'initial-states.npz'))
        self.io={}
        for role,engine in [('encoder',self.encoder),('decoder',self.decoder)]:
            self.io[role]=[{ 'name':engine.get_tensor_name(i),'mode':str(engine.get_tensor_mode(engine.get_tensor_name(i))),'shape':list(engine.get_tensor_shape(engine.get_tensor_name(i))),'dtype':str(engine.get_tensor_dtype(engine.get_tensor_name(i))),'location':str(engine.get_tensor_location(engine.get_tensor_name(i))),'shapeInferenceIo':engine.is_shape_inference_io(engine.get_tensor_name(i))} for i in range(engine.num_io_tensors)]
        print(json.dumps({'engineIo':self.io}),flush=True)
        self.enc_inputs=[v['name'] for v in self.io['encoder'] if v['mode'].endswith('INPUT')]
        assert len(self.enc_inputs)==1
        self.image_name=self.enc_inputs[0]
        self.image_shape=tuple(self.encoder.get_tensor_shape(self.image_name))
        self.image=self.alloc(math.prod(self.image_shape)*4)
        self.enc.set_input_shape(self.image_name,self.image_shape)
        self.enc.set_tensor_address(self.image_name,self.image.ptr)
        self.invariants={}
        for n in self.meta['invariants']:
            shape=tuple(self.enc.get_tensor_shape(n)); dtype=np.dtype(trt.nptype(self.encoder.get_tensor_dtype(n)))
            assert all(d>=0 for d in shape) and self.encoder.get_tensor_location(n)==trt.TensorLocation.DEVICE
            buf=self.alloc(math.prod(shape)*dtype.itemsize)
            self.enc.set_tensor_address(n,buf.ptr); self.invariants[n]=(buf,shape)
        self.start_states={}
        for n,a in self.initial.items():
            spec=self.meta['profiles'][n]
            shape=spec['max']; size=math.prod(shape)*a.dtype.itemsize
            buf=self.alloc(size,host=self.decoder.get_tensor_location(n)==trt.TensorLocation.HOST)
            if buf.host: assert size<=256, 'host token/cache tensors prohibited'
            self.start_states[n]=buf
        for n,spec in self.meta['profiles'].items():
            self.dec.set_input_shape(n,spec['max'])
            self.dec.set_tensor_address(n,self.invariants[n][0].ptr if n in self.invariants else self.start_states[n].ptr)
        self.output_banks=[]; self.allocators=[]
        outputs=[v['name'] for v in self.io['decoder'] if v['mode'].endswith('OUTPUT')]
        reverse={s['output']:s['input'] for s in self.meta['states']}
        for bank in range(2):
            buffers={}; allocators={}
            for n in outputs:
                dtype=np.dtype(trt.nptype(self.decoder.get_tensor_dtype(n)))
                if n in reverse:
                    shape=list(self.meta['profiles'][reverse[n]]['max'])
                    for axis,d in enumerate(shape):
                        if d>128 and axis in (1,2): shape[axis]+=16
                    capacity=math.prod(shape)*dtype.itemsize+1024
                else: capacity=1024
                host=self.decoder.get_tensor_location(n)==trt.TensorLocation.HOST
                assert not host or capacity<=4096
                buf=self.alloc(capacity,host=host)
                buffers[n]=buf; allocators[n]=OutputAllocator(buf)
            self.output_banks.append(buffers); self.allocators.append(allocators)
        self.condition_host=self.alloc(256,host=True)
        self.tokens_host=self.alloc((self.meta['maxSequenceLength']+32)*8,host=True)
        self.host_control={}
        for state in self.meta['states']:
            n=state['input']; out=state['output']
            in_host=self.decoder.get_tensor_location(n)==trt.TensorLocation.HOST
            out_host=self.decoder.get_tensor_location(out)==trt.TensorLocation.HOST
            if in_host!=out_host:
                spec=self.meta['profiles'][n]
                size=math.prod(spec['max'])*np.dtype(spec['dtype']).itemsize
                assert size<=256, 'cache/image host readback prohibited'
                self.host_control[n]=[self.alloc(size,host=in_host) for _ in range(2)]
        print(json.dumps({'ownedBufferBytes':sum(b.capacity for b in self.buffers),'hostMetadataBridges':list(self.host_control)}),flush=True)
    def alloc(self,size,host=False):
        b=Buffer(size,host); self.buffers.append(b); return b
    def copy(self,dest,src,size):
        if size: ck(cu.cudaMemcpyAsync(dest,src,size,cu.cudaMemcpyKind.cudaMemcpyDefault,self.stream))
    def run(self,x):
        assert self.healthy and x.dtype==np.float32 and x.shape==self.image_shape and x.flags.c_contiguous
        started=time.perf_counter()
        try:
            self.copy(self.image.ptr,x.ctypes.data,x.nbytes)
            assert self.enc.execute_async_v3(self.stream)
            states={}
            for n,a in self.initial.items():
                buf=self.start_states[n]
                if buf.host: ctypes.memmove(buf.ptr,a.ctypes.data,a.nbytes)
                else: self.copy(buf.ptr,a.ctypes.data,a.nbytes)
                states[n]=(buf,tuple(a.shape))
            for n,(buf,shape) in self.invariants.items():
                assert self.dec.set_input_shape(n,shape)
                assert self.dec.set_tensor_address(n,buf.ptr)
            for iteration in range(self.meta['maxSequenceLength']+8):
                bank=iteration%2
                for n,(buf,shape) in states.items():
                    assert self.dec.set_input_shape(n,shape)
                    assert self.dec.set_tensor_address(n,buf.ptr)
                for n,buf in self.output_banks[bank].items():
                    alloc=self.allocators[bank][n]; alloc.shape=None; alloc.refused=None
                    self.dec.set_output_allocator(n,alloc)
                    assert self.dec.set_tensor_address(n,buf.ptr)
                assert self.dec.execute_async_v3(self.stream), 'TensorRT decoder execution failed'
                cond=self.meta['conditionOutput']; source=self.output_banks[bank][cond]
                if source.host:
                    ck(cu.cudaStreamSynchronize(self.stream)); keep_going=bool(source.host_view(np.bool_,(1,))[0])
                else:
                    self.copy(self.condition_host.ptr,source.ptr,1)
                    ck(cu.cudaStreamSynchronize(self.stream)); keep_going=bool(self.condition_host.host_view(np.bool_,(1,))[0])
                for n,a in self.allocators[bank].items():
                    if a.refused: raise RuntimeError('output bound/alignment refusal '+str(a.refused))
                def output_shape(n):
                    value=self.allocators[bank][n].shape
                    if value is None: value=tuple(self.dec.get_tensor_shape(n))
                    assert all(d>=0 for d in value), ('unresolved output shape',n,value)
                    return value
                if not keep_going:
                    n=self.meta['finalTokenOutput']; shape=output_shape(n)
                    assert len(shape)==2 and shape[0]==1 and shape[1]<=self.meta['maxSequenceLength']+8
                    self.copy(self.tokens_host.ptr,self.output_banks[bank][n].ptr,math.prod(shape)*8)
                    ck(cu.cudaStreamSynchronize(self.stream))
                    tokens=self.tokens_host.host_view(np.int64,shape).copy()
                    return tokens,{'modelCallMs':(time.perf_counter()-started)*1000,'iterations':iteration+1,'tokenCount':tokens.size}
                states={}
                for s in self.meta['states']:
                    n,out=s['input'],s['output']; shape=output_shape(out); buf=self.output_banks[bank][out]
                    if n in self.host_control:
                        new=self.host_control[n][bank]
                        size=math.prod(shape)*np.dtype(self.meta['profiles'][n]['dtype']).itemsize
                        self.copy(new.ptr,buf.ptr,size); buf=new
                    states[n]=(buf,shape)
                # Host shape inputs cannot be consumed until their tiny D2H is complete.
                if self.host_control: ck(cu.cudaStreamSynchronize(self.stream))
            raise RuntimeError('decoder exceeded original model token bound')
        except BaseException:
            self.healthy=False
            # Unknown GPU completion is quarantined until this process exits.
            raise
    def close(self):
        if not self.healthy: return
        ck(cu.cudaStreamSynchronize(self.stream))
        self.enc=None; self.dec=None
        for b in reversed(self.buffers): b.free()
        ck(cu.cudaStreamDestroy(self.stream))


def main():
    p=argparse.ArgumentParser(parents=[BOOT]); p.add_argument('--work-dir',type=Path,required=True); p.add_argument('model'); p.add_argument('--partition',default='partition-v2'); p.add_argument('--warmup',type=int,default=3); p.add_argument('--repeat',type=int,default=12); p.add_argument('--case',default=None); p.add_argument('--output',required=True)
    a=p.parse_args(); root=a.work_dir.resolve()
    if a.warmup<0 or a.repeat<1: p.error("warmup >= 0 and repeat >= 1 required")
    output=Path(a.output); assert not output.exists(), 'preserve prior benchmark'
    start=time.perf_counter(); runtime=FormulaRuntime(root/a.partition/a.model); load=time.perf_counter()-start
    cases=['chalk-kinetic-normalized','chalk-work-normalized','chalk-delta-normalized','official-complex'] if a.case is None else [a.case]
    receipt={'platform':platform.platform(),'backend':'native Windows TensorRT only; no Paddle/ORT runtime fallback','model':a.model,'trtVersion':trt.__version__,'precision':runtime.precision,'loadSeconds':load,'warmup':a.warmup,'repeat':a.repeat,'scope':'Preprocessed tensor H2D + encoder + bounded autoregressive decoder + control synchronization + final tokens D2H; excludes preprocessing and LaTeX detokenization.','engineIo':runtime.io,'cases':[]}
    try:
        for case in cases:
            x=np.load(root/'reference'/a.model/(case+'-input.npy'))
            expected=np.load(root/'reference'/a.model/(case+'-tokens.npy'))
            runs=[]
            for iteration in range(a.warmup+a.repeat):
                tokens,stats=runtime.run(x)
                stats['tokensEqualPaddle']=bool(np.array_equal(tokens,expected))
                if not stats['tokensEqualPaddle']: print('TOKEN DIFFERENCE',case,tokens.tolist(),expected.tolist(),flush=True)
                if iteration>=a.warmup: runs.append(stats)
            entry={'case':case,'inputSha256':hashlib.sha256(x.tobytes()).hexdigest(),'runs':runs,'medianMs':statistics.median(r['modelCallMs'] for r in runs),'allTokensEqualPaddle':all(r['tokensEqualPaddle'] for r in runs),'tokens':tokens.tolist()}
            receipt['cases'].append(entry); print(json.dumps(entry),flush=True)
            output.write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    finally: runtime.close()
    receipt['completed']=True
    receipt['allTokensEqualPaddle']=all(v['allTokensEqualPaddle'] for v in receipt['cases'])
    output.write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    if not receipt['allTokensEqualPaddle']: raise RuntimeError('TensorRT token mismatch; measurements retained as a differing-output result')
    print('BENCHMARK COMPLETE',flush=True)
if __name__=='__main__': main()
