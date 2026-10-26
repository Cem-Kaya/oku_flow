"""Bounded binding/cache helpers; API injected for CPU-only contract tests."""
class EncoderReplay:
    def __init__(self,context,api): self.api=api;self.context=context;self.graph=None;self.executable=None;self.launches=0
    def execute_async_v3(self,stream):
        if self.executable is None:
            if not self.context.execute_async_v3(stream): return False
            self.api.ck(self.api.cu.cudaStreamSynchronize(stream))
            self.api.ck(self.api.cu.cudaStreamBeginCapture(stream,self.api.cu.cudaStreamCaptureMode.cudaStreamCaptureModeThreadLocal))
            if not self.context.execute_async_v3(stream): raise RuntimeError('encoder graph capture execution failed')
            self.graph=self.api.ck(self.api.cu.cudaStreamEndCapture(stream))
            self.executable=self.api.ck(self.api.cu.cudaGraphInstantiate(self.graph,0))
        else:
            self.api.ck(self.api.cu.cudaGraphLaunch(self.executable,stream));self.launches+=1
        return True
    def destroy(self):
        if self.executable is not None: self.api.ck(self.api.cu.cudaGraphExecDestroy(self.executable));self.executable=None
        if self.graph is not None: self.api.ck(self.api.cu.cudaGraphDestroy(self.graph));self.graph=None
    def __getattr__(self,name): return getattr(self.context,name)


class DecoderBindings:
    def __init__(self,contexts,allocators):
        self.contexts=contexts;self.bank=0
        self.allocator_bank={id(a):i for i,group in enumerate(allocators) for a in group.values()}
        self.shapes={};self.addresses={};self.allocators={}
        self.applied=[{'shapes':{},'addresses':{},'allocators':{}} for _ in contexts]
        self.writes=0;self.skipped=0
    def set_input_shape(self,name,shape): self.shapes[name]=tuple(shape);return True
    def set_tensor_address(self,name,address): self.addresses[name]=address;return True
    def set_output_allocator(self,name,allocator):
        self.bank=self.allocator_bank[id(allocator)];self.allocators[name]=allocator
    def execute_async_v3(self,stream):
        context=self.contexts[self.bank];applied=self.applied[self.bank]
        for category,values,method in [('shapes',self.shapes,context.set_input_shape),('addresses',self.addresses,context.set_tensor_address),('allocators',self.allocators,context.set_output_allocator)]:
            for name,value in values.items():
                if applied[category].get(name)==value: self.skipped+=1;continue
                ok=method(name,value)
                if ok is False: raise RuntimeError('refused decoder binding '+name)
                applied[category][name]=value;self.writes+=1
        return context.execute_async_v3(stream)
    def get_tensor_shape(self,name): return self.contexts[self.bank].get_tensor_shape(name)
