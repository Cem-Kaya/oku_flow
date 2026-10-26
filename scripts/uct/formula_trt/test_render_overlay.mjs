import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {validateRegion,makeRenderer,render} from './render_overlay.mjs';

test('bounded region requires integer source coordinates and permitted TeX',()=>{
  assert.deepEqual(validateRegion({sourceLTRB:[1,2,8,9],latex:'x^{2}'},10,10),{left:1,top:2,width:7,height:7});
  for(const q of [[-1,0,3,4],[1,0,11,4],[3,0,2,4],[0,0,NaN,4],[0,0,3.5,4]]) assert.throws(()=>validateRegion({sourceLTRB:q,latex:'x'},10,10));
  assert.throws(()=>validateRegion({sourceLTRB:[0,0,2,2],latex:'\\href{https://example.com}{x}'},10,10));
});
test('changed source refuses before rendering or writing output',async()=>{
  const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'okuflow-formula-render-'));
  try {
    const src=path.join(tmp,'source.png'),request=path.join(tmp,'request.json'),output=path.join(tmp,'output');
    fs.writeFileSync(src,'changed');fs.writeFileSync(request,JSON.stringify({version:1,regions:[],source:src,sourceSha256:'wrong'}));
    await assert.rejects(render(request,output,'unused'),/changed/);assert.equal(fs.existsSync(output),false);
  } finally {const resolved=fs.realpathSync(tmp);assert.equal(path.dirname(resolved).toLowerCase(),fs.realpathSync(os.tmpdir()).toLowerCase());assert.ok(path.basename(resolved).startsWith('okuflow-formula-render-'));fs.rmSync(resolved,{recursive:true,force:true});}
});
test('actual fraction and superscript produce vector paths and cache reuse',()=>{
  assert.ok(process.env.OKUFLOW_FORMULA_RENDER_DEPS,'explicit private dependency root required');
  const renderer=makeRenderer(process.env.OKUFLOW_FORMULA_RENDER_DEPS);
  const first=renderer.typeset('k=\\frac{1}{2}mv^{2}'),second=renderer.typeset('k=\\frac{1}{2}mv^{2}');
  assert.equal(first.cacheHit,false);assert.equal(second.cacheHit,true);assert.equal(first.content,second.content);assert.match(first.content,/<path/);assert.ok(first.viewBox[2]>0);
  assert.throws(()=>renderer.typeset('\\notARealCommand{x}'),/failed/);
});
