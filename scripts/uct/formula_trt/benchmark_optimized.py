"""Same formula math; replay the fixed encoder and cache two decoder binding sets."""
import json
import sys
from pathlib import Path
import benchmark as base


from execution_optimizations import EncoderReplay, DecoderBindings


LATEST=None
class OptimizedFormulaRuntime(base.FormulaRuntime):
    def __init__(self,directory):
        super().__init__(directory)
        self.replay=EncoderReplay(self.enc,base);self.enc=self.replay
        self.bindings=DecoderBindings([self.dec,self.decoder.create_execution_context()],self.allocators)
        self.dec=self.bindings
        global LATEST;LATEST=self
    def close(self):
        if not self.healthy: return
        base.ck(base.cu.cudaStreamSynchronize(self.stream));self.replay.destroy()
        super().close()


if __name__=='__main__':
    base.FormulaRuntime=OptimizedFormulaRuntime
    try: base.main()
    finally:
        if LATEST is not None and '--output' in sys.argv:
            output=Path(sys.argv[sys.argv.index('--output')+1])
            if output.exists():
                result=json.loads(output.read_text(encoding='utf-8'))
                result['optimization']={'encoderCudaGraphReplays':LATEST.replay.launches,'decoderContexts':2,'bindingWrites':LATEST.bindings.writes,'redundantBindingsSkipped':LATEST.bindings.skipped,'scope':'same graphs and tokens; stable encoder addresses; distinct decoder output banks'}
                output.write_text(json.dumps(result,indent=2),encoding='utf-8')
