// First-party opt-in independent-client acceptance against AppControlTarget.
// Requires the private offscreen fixture; never launches the owner's app.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile,realpath,mkdir,writeFile} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {resolve,dirname,join,basename} from 'node:path';
import {pathToFileURL} from 'node:url';

const args=new Map();
assert.equal((process.argv.length-2)%2,0,'Arguments must be named pairs');
for(let i=2;i<process.argv.length;i+=2){
    assert(['--sdk-root','--app-fixture','--bridge','--evidence-dir'].includes(process.argv[i]));
    assert(!args.has(process.argv[i]));args.set(process.argv[i],process.argv[i+1]);
}
for(const key of ['--sdk-root','--app-fixture','--bridge','--evidence-dir'])assert(args.get(key),`Missing ${key}`);
const sdkRoot=await realpath(args.get('--sdk-root'));
const sdk=join(sdkRoot,'node_modules','@modelcontextprotocol','sdk');
assert.equal(JSON.parse(await readFile(join(sdk,'package.json'),'utf8')).version,'1.32.1');
const lock=JSON.parse(await readFile(join(sdkRoot,'package-lock.json'),'utf8'));
assert.equal(lock.packages['node_modules/@modelcontextprotocol/sdk'].integrity,
    'sha512-2DdE+SJDtzLEEWzY1ZjY7Q+VcPhcV1KisD3zI4u0XZyktsjHum1mwbMI+JaulUBi2OZk+KJAi2uPXzxichPkdw==');
const {Client}=await import(pathToFileURL(join(sdk,'dist','esm','client','index.js')).href);
const {StdioClientTransport}=await import(pathToFileURL(join(sdk,'dist','esm','client','stdio.js')).href);
const fixture=await realpath(args.get('--app-fixture')),bridge=await realpath(args.get('--bridge'));
const local=resolve(process.env.LOCALAPPDATA),folder=resolve(args.get('--evidence-dir'));
for(const file of [fixture,bridge,folder])assert(file.toLowerCase().startsWith(local.toLowerCase()+'\\'),
    'Native fixture and evidence must stay in LocalAppData');
assert.equal(dirname(fixture),dirname(bridge));assert.equal(basename(dirname(fixture)),'native-control');
assert(['oku_flow.exe','OkuFlowApp.exe'].includes(basename(fixture)));assert.equal(basename(bridge),'okuflow_mcp.exe');
const app=spawn(fixture,['--stdio-fixture'],{stdio:['pipe','pipe','pipe'],windowsHide:true,
    env:{...process.env,QT_QPA_PLATFORM:'offscreen',OKUFLOW_VLM_API_KEY:''}});
let appErrors='';app.stderr.on('data',b=>{appErrors+=b.toString();});
const appExit=new Promise((resolve,reject)=>{app.once('error',reject);app.once('exit',(code,signal)=>resolve({code,signal}));});
const lines=createInterface({input:app.stdout}),buffered=[],waiting=[];
lines.on('line',line=>{let data;try{data=JSON.parse(line);}catch(error){
    for(const waiter of waiting.splice(0)){clearTimeout(waiter.timer);waiter.reject(error);}return;}
    const index=waiting.findIndex(w=>w.type===data.type);
    if(index<0)buffered.push(data);else{const [waiter]=waiting.splice(index,1);clearTimeout(waiter.timer);waiter.resolve(data);}});
const next=type=>{const index=buffered.findIndex(x=>x.type===type);if(index>=0)return Promise.resolve(buffered.splice(index,1)[0]);
    return new Promise((resolve,reject)=>{const item={type,resolve,reject};item.timer=setTimeout(()=>{
        waiting.splice(waiting.indexOf(item),1);reject(new Error(`native fixture timeout: ${type}`));},10000);waiting.push(item);});};
const command=async(text,type)=>{app.stdin.write(`${text}\n`);return next(type);};
const scenarios=[];const check=async(name,fn)=>{const row={name,started:Date.now()};scenarios.push(row);
    try{await fn();row.status='passed';}catch(error){row.status='failed';row.error=String(error);throw error;}
    finally{row.durationMs=Date.now()-row.started;}};
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
let client,peerId,catalogEdition=null;
const call=(name,arguments_={})=>client.callTool({name,arguments:arguments_});
const success=result=>{assert.equal(result.isError,false);assert.equal(result.structuredContent.ok,true);
    assert.deepEqual(JSON.parse(result.content[0].text),result.structuredContent);
    assert(Buffer.byteLength(JSON.stringify(result.structuredContent))<=32768);return result.structuredContent;};
const failure=(result,code)=>{assert.equal(result.isError,true);assert.equal(result.structuredContent,undefined);
    assert.equal(result.content.length,1);assert.equal(result.content[0].type,'text');const dto=JSON.parse(result.content[0].text);
    assert.equal(dto.ok,false);if(code)assert.equal(dto.error.code,code);return dto;};
const mutation=async(name,values)=>{await pause(550);return success(await call(name,values));};
const nativeState=async()=>{const state=await command('native-state','native-state');
    assert.equal(state.realTarget,true);assert.equal(state.platformInitialized,false);assert.equal(state.cameraActive,false);
    assert.equal(state.cameraCount,0);assert.equal(state.recording,false);return state;};
try{
    await check('actual production target and independent stdio lease admission',async()=>{
        const ready=await next('ready');assert.equal(ready.realTarget,true);assert(['dev-full','v1'].includes(ready.catalogEdition));
        catalogEdition=ready.catalogEdition;
        assert.match(ready.selector,/^[a-f0-9]{32}$/);await nativeState();
        const transport=new StdioClientTransport({command:bridge,args:['--instance',ready.selector],stderr:'pipe'});
        client=new Client({name:'OkuFlow native SDK acceptance',version:'1.0'},{capabilities:{}});await client.connect(transport);
        assert.equal(client.getServerVersion().version,catalogEdition);const list=await client.listTools();assert.equal(list.tools.length,43);
        for(const tool of list.tools){const additive=['capture.photo.take','capture.recording.start','video.save_frame','view.observe'].includes(tool.name);
            assert.equal(tool.annotations.destructiveHint,!(tool.annotations.readOnlyHint||additive));}
        peerId=(await next('pending')).peerId;assert.match(peerId,/^[a-f0-9]{32}$/);
        failure(await call('app.state.get'));assert.equal((await command(`approve:${peerId}`,'approved')).ok,true);
    });
    await check('bounded native discovery separates support from availability',async()=>{
        const ids=new Set();let afterField,parameters=0,protectedItems=0,actions=0;
        for(let page=0;page<20;++page){const result=success(await call('app.controls.list',{limit:16,...(afterField?{afterField}:{})}));
            assert(result.items.length<=16);for(const item of result.items){assert(!ids.has(item.id));ids.add(item.id);
                assert.equal(item.value,undefined);if(item.kind==='parameter'){++parameters;
                    if(item.available===false)assert(item.unsupportedReason,'Unavailable control must explain why');
                }else if(item.kind==='protected'){++protectedItems;assert.equal(item.requiresUi,true);assert.equal(item.writable,false);}
                else ++actions;}
            if(!result.hasMore)break;assert(result.afterField);assert.notEqual(result.afterField,afterField);afterField=result.afterField;}
        assert.equal(parameters,98);assert.equal(protectedItems,17);assert.equal(actions,46);
    });
    await check('all eight native grouped reads without bootstrap or hardware',async()=>{
        for(const group of ['image.settings','view.settings','camera.settings','recording.settings','assistant.preferences',
            'notes.preferences','ui.preferences','annotation.preferences']){
            const result=success(await call(`${group}.get`));assert.equal(typeof result.parameters,'object');}
        assert.equal(success(await call('camera.sources.list')).sources.length,0);await nativeState();
    });
    await check('native grouped and legacy zoom handlers and private saved settings',async()=>{
        const id='view.zoom.amount';success(await call('view.settings.get',{fields:[id]}));
        await mutation('view.settings.set',{[id]:2});let state=await nativeState();
        assert.equal(state.zoomSlider,200);assert.equal(state.savedZoom,2);
        await mutation('view.zoom.set',{magnification:3});state=await nativeState();
        assert.equal(state.zoomSlider,300);assert.equal(state.savedZoom,3);
        const row=success(await call('view.settings.get',{fields:[id]})).parameters[id];
        assert.equal(row.requested,3);assert.equal(row.saved,3);
    });
    await check('native user revision rejects the stale agent write',async()=>{
        await command('native-zoom','native-changed');await pause(550);
        failure(await call('view.settings.set',{'view.zoom.amount':2.5}),'stale_state');
        assert.equal((await nativeState()).zoomSlider,175);
    });
    await check('cold image hardware availability and strict field admission',async()=>{
        const before=await nativeState();
        const row=success(await call('image.settings.get',{fields:['image.contrast']})).parameters['image.contrast'];
        assert.equal(row.available,false);assert.equal(row.unsupportedReason,'cuda_unavailable');
        await pause(550);failure(await call('image.settings.set',{'image.contrast':1.5}),'unavailable');
        assert.equal((await nativeState()).contrastSlider,before.contrastSlider);
        failure(await call('view.settings.set',{'view.zoom.amount':2,undeclared:true}),'invalid_argument');
        failure(await call('assistant.preferences.set',{'assistant.apiKey':'never'}),'invalid_argument');
    });
    await check('exact trusted approval applies only the requested native preference',async()=>{
        const id='annotation.enabled';success(await call('annotation.preferences.get',{fields:[id]}));await pause(550);
        const asked=failure(await call('annotation.preferences.set',{[id]:true}),'consent_required');assert(asked.approvalId);
        const resolved=await command(`resolve:${asked.approvalId}`,'resolved');assert.equal(resolved.result.ok,true);
        assert.equal(success(await call('annotation.preferences.get',{fields:[id]})).parameters[id].requested,true);
        const replay=await command(`resolve:${asked.approvalId}`,'resolved');assert.equal(replay.result.ok,false);
        assert.equal(replay.result.error.code,'not_found');await nativeState();
    });
    await check('native undo preserves user state and retires the external lease',async()=>{
        failure(await call('view.observe'),'denied_by_policy');failure(await call('capture.photo.take'),'denied_by_policy');
        failure(await call('capture.recording.start',{audio:'none'}),'denied_by_policy');
        const undone=await command('undo','undo');assert.equal(undone.result.ok,true);
        assert.equal((await nativeState()).zoomSlider,175);failure(await call('app.state.get'));
        await command('revoke','revoked');failure(await call('app.state.get'));await nativeState();
    });
    await check('normal EOF cleanup of exact private native child',async()=>{
        await client.close();client=null;app.stdin.end('exit\n');let timer;
        try{const exit=await Promise.race([appExit,new Promise((_,reject)=>timer=setTimeout(()=>reject(new Error('native exit timeout')),8000))]);
            assert.equal(exit.code,0);assert.equal(exit.signal,null);}finally{clearTimeout(timer);}
    });
    console.log(`PASS: ${scenarios.length} actual AppControlTarget SDK scenarios; offscreen CPU subset, no hardware/provider acceptance.`);
}finally{
    if(client)await client.close().catch(()=>{});if(app.exitCode===null)app.stdin.end('exit\n');
    lines.close();for(const waiter of waiting.splice(0))clearTimeout(waiter.timer);
    await mkdir(folder,{recursive:true});const escape=s=>String(s).replace(/[<>&"']/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;',"'":'&apos;'}[c]));
    await writeFile(join(folder,'mcp-native-sdk-evidence.json'),JSON.stringify({sdk:'1.32.1',catalog:catalogEdition,
        synthetic:false,target:'AppControlTarget',offscreen:true,hardwareAcceptance:false,providerAcceptance:false,
        fullEffectsAcceptance:false,scenarios,fixtureStderr:appErrors},null,2)+'\n');
    await writeFile(join(folder,'mcp-native-sdk-junit.xml'),`<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="MCP actual native target offscreen SDK" tests="${scenarios.length}" failures="${scenarios.filter(s=>s.status==='failed').length}">${scenarios.map(s=>`<testcase name="${escape(s.name)}" time="${s.durationMs/1000}">${s.status==='failed'?`<failure message="${escape(s.error)}"/>`:''}</testcase>`).join('')}</testsuite>\n`);
}
