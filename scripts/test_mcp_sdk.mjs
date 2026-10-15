// First-party opt-in, desktop-free interoperability check. Install the pinned
// development-only SDK outside Drive/Git; never points at the owner's app.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile, realpath, mkdir, writeFile} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {resolve, dirname, join} from 'node:path';
import {pathToFileURL} from 'node:url';

const args=new Map();
for(let i=2;i<process.argv.length;i+=2) args.set(process.argv[i],process.argv[i+1]);
for(const key of ['--sdk-root','--app-fixture','--bridge']) assert(args.get(key),`Missing ${key}`);
const sdkRoot=await realpath(args.get('--sdk-root'));
const sdk=join(sdkRoot,'node_modules','@modelcontextprotocol','sdk');
const manifest=JSON.parse(await readFile(join(sdk,'package.json'),'utf8'));
assert.equal(manifest.version,'1.32.1');
const lock=JSON.parse(await readFile(join(sdkRoot,'package-lock.json'),'utf8'));
assert.equal(lock.packages['node_modules/@modelcontextprotocol/sdk'].integrity,
    'sha512-2DdE+SJDtzLEEWzY1ZjY7Q+VcPhcV1KisD3zI4u0XZyktsjHum1mwbMI+JaulUBi2OZk+KJAi2uPXzxichPkdw==');
const {Client}=await import(pathToFileURL(join(sdk,'dist','esm','client','index.js')).href);
const {StdioClientTransport}=await import(pathToFileURL(join(sdk,'dist','esm','client','stdio.js')).href);
const fixture=await realpath(args.get('--app-fixture'));
const bridge=await realpath(args.get('--bridge'));
// Fixed private build fixture directory contract, not a generic app launcher.
assert.equal(dirname(fixture),dirname(bridge));
assert.equal(dirname(fixture).split(/[\\/]/).at(-1),'mcp-fixture');
const app=spawn(fixture,[],{stdio:['pipe','pipe','pipe'],windowsHide:true});
let appErrors='';app.stderr.on('data',b=>{appErrors+=(b.toString());});
const lines=createInterface({input:app.stdout});
const buffered=[];const waiting=[];
lines.on('line',line=>{const data=JSON.parse(line);const slot=waiting.findIndex(w=>w.type===data.type);
    if(slot>=0){const [waiter]=waiting.splice(slot,1);clearTimeout(waiter.timer);waiter.resolve(data);}
    else buffered.push(data);});
const next=type=>{const index=buffered.findIndex(x=>x.type===type);if(index>=0)return Promise.resolve(buffered.splice(index,1)[0]);
    return new Promise((resolve,reject)=>{const item={type,resolve};item.timer=setTimeout(()=>{waiting.splice(waiting.indexOf(item),1);reject(new Error(`fixture timeout: ${type}`));},10000);waiting.push(item);});};
const appExit=new Promise((resolve,reject)=>{app.once('error',reject);app.once('exit',(code,signal)=>resolve({code,signal}));});
let client,transport,exitTimer,current;
const evidence=[];
const check=async(name,fn)=>{current={name,started:Date.now()};evidence.push(current);
    try{await fn();current.status='passed';}catch(error){current.status='failed';current.error=String(error);throw error;}
    finally{current.durationMs=Date.now()-current.started;}};
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const command=async(text,type)=>{app.stdin.write(`${text}\n`);return next(type);};
const call=async(name,arguments_={})=>client.callTool({name,arguments:arguments_});
const success=result=>{assert.equal(result.isError,false);assert.equal(result.structuredContent.ok,true);
    assert.deepEqual(JSON.parse(result.content[0].text),result.structuredContent);return result.structuredContent;};
const failure=(result,code)=>{assert.equal(result.isError,true);assert.equal(result.structuredContent,undefined);
    assert.equal(result.content.length,1);assert.equal(result.content[0].type,'text');
    const dto=JSON.parse(result.content[0].text);assert.equal(dto.ok,false);if(code)assert.equal(dto.error.code,code);return dto;};
const mutation=async(name,arguments_={})=>{await pause(550);return success(await call(name,arguments_));};
const operation=async id=>success(await call('app.operation.get',{operationId:id}));
const settle=()=>command('settle','settled');
let peerId,edition,hasCapture,hasCamera;
const lease=async grant=>{await command('revoke','revoked');
    const approval=await command(`approve${grant}:${peerId}`,'approved');assert.equal(approval.ok,true);};
try {
    await check('initialize and immutable catalog',async()=>{
        const ready=await next('ready');assert.match(ready.selector,/^[a-f0-9]{32}$/);edition=ready.catalogEdition;
        hasCapture=edition!=='dev-core';hasCamera=edition==='dev-camera'||edition==='v1'||edition==='dev-full';
        transport=new StdioClientTransport({command:bridge,args:['--instance',ready.selector],stderr:'pipe'});
        client=new Client({name:'OkuFlow SDK fixture',version:'1.0'},{capabilities:{}});await client.connect(transport);
        const list=await client.listTools();assert.equal(client.getServerVersion().version,edition);
        assert.equal(list.tools.length,{'dev-core':14,'dev-capture':17,'dev-camera':19,'v1':36,'dev-full':36}[edition]);
        for(const tool of list.tools){assert.equal(tool.inputSchema.$schema,'http://json-schema.org/draft-07/schema#');
            assert.equal(tool.outputSchema.type,'object');assert(tool.outputSchema.oneOf);assert.equal(tool.annotations.openWorldHint,false);
            const additive=['capture.photo.take','capture.recording.start','view.observe'].includes(tool.name);
            assert.equal(tool.annotations.destructiveHint,!(tool.annotations.readOnlyHint||additive));}
        const pending=await next('pending');peerId=pending.peerId;assert.match(peerId,/^[a-f0-9]{32}$/);
        failure(await call('app.state.get'));
        assert.equal((await command(`approve:${peerId}`,'approved')).ok,true);
    });
    if(edition==='dev-full'){
        await check('expanded immutable metadata pages and protected categories',async()=>{
            let afterField;const ids=new Set();let fields=0,protectedEntries=0,actions=0;
            for(let page=0;page<20;++page){
                const result=success(await call('app.controls.list',{limit:16,...(afterField?{afterField}:{})}));
                assert.equal(result.catalogEdition,'dev-full');assert(result.items.length<=16);
                assert(Buffer.byteLength(JSON.stringify(result))<=32*1024);
                for(const item of result.items){assert(!ids.has(item.id));ids.add(item.id);assert.equal(item.value,undefined);
                    if(item.kind==='parameter'){++fields;assert.equal(item.implementation,'target-integration-required');}
                    else if(item.kind==='protected'){++protectedEntries;assert.equal(item.requiresUi,true);assert.equal(item.writable,false);assert.equal(item.schema,undefined);}
                    else{++actions;assert(['existing-tool-runtime-gated','native-ui-only'].includes(item.status));}}
                if(!result.hasMore)break;assert(result.afterField);assert.notEqual(result.afterField,afterField);afterField=result.afterField;
            }
            assert.equal(fields,94);assert.equal(protectedEntries,17);assert.equal(actions,39);
            failure(await call('app.controls.list',{afterField:'unknown.field'}),'invalid_argument');
        });
        await check('expanded schema admission with explicit synthetic read subset',async()=>{
            const view=success(await call('view.settings.get',{fields:['view.zoom.amount']}));
            assert.equal(view.support,'synthetic-read-subset');assert.equal(view.parameters['view.zoom.amount'],1);
            failure(await call('view.settings.get',{fields:['view.zoom.amount','view.zoom.amount']}),'invalid_argument');
            failure(await call('image.settings.set',{}),'invalid_argument');
            failure(await call('image.settings.set',{'image.colors.custom.stops':['#000000']}),'invalid_argument');
            failure(await call('assistant.preferences.set',{'assistant.apiKey':'never'}),'invalid_argument');
            failure(await call('image.settings.set',{'image.blur.enabled':true}),'unavailable');
            assert.equal((await command('fixture-status','fixture-status')).writes,0);
        });
        await check('bounded assistant projections preserve complete4096Unicode values',async()=>{
            const id='assistant_preferences.instructions';const fields=[id];
            failure(await call('assistant.preferences.get',{fields,valueKind:'effective'}),'invalid_argument');
            failure(await call('assistant.preferences.get',{fields,valueKind:null}),'invalid_argument');
            failure(await call('view.settings.get',{valueKind:'saved'}),'invalid_argument');
            const all=success(await call('assistant.preferences.get',{fields}));
            assert.equal(all.support,'synthetic-assistant-projection');assert.equal(all.parameters[id].filterRequired,true);
            assert.equal(all.parameters[id].unsupportedReason,'response_budget');assert.equal(all.parameters[id].requested,null);
            assert.equal(all.fieldRevisions,undefined);assert(Buffer.byteLength(JSON.stringify(all))<=32*1024);
            const requested=success(await call('assistant.preferences.get',{fields,valueKind:'requested'}));
            assert.equal(requested.parameters[id].requested,'\u{1f9ea}'.repeat(4096));
            assert.equal(requested.parameters[id].effective,undefined);assert.equal(requested.parameters[id].saved,undefined);
            assert.equal(Object.keys(requested.fieldRevisions).length,1);assert(Buffer.byteLength(JSON.stringify(requested))<=32*1024);
            const saved=success(await call('assistant.preferences.get',{fields,valueKind:'saved'}));
            assert.equal(saved.parameters[id].saved,'\u{1f9ed}'.repeat(4096));
            assert.equal(saved.parameters[id].requested,undefined);assert.equal(saved.parameters[id].effective,undefined);
            assert.equal(saved.fieldRevisions,undefined);assert(Buffer.byteLength(JSON.stringify(saved))<=32*1024);
            await command('assistant-equal','assistant-profile-set');
            const equal=success(await call('assistant.preferences.get',{fields,valueKind:'all'}));
            assert.equal(equal.parameters[id].requested,'\u{1f9ea}'.repeat(4096));
            assert.deepEqual(equal.parameters[id].effective,{sameAs:'requested'});
            assert.deepEqual(equal.parameters[id].saved,{sameAs:'requested'});assert.equal(equal.parameters[id].filterRequired,undefined);
            await command('assistant-short','assistant-profile-set');
            const short=success(await call('assistant.preferences.get',{fields}));
            assert.equal(short.parameters[id].requested,'Synthetic instructions');
            assert.equal(short.parameters[id].effective,'Synthetic instructions');assert.equal(short.parameters[id].saved,'Synthetic instructions');
            await command('assistant-distinct','assistant-profile-set');
        });
    }
    await check('state zoom center reset and safe queries',async()=>{
        assert.equal(success(await call('app.state.get')).fixture,true);
        assert.equal((await mutation('view.zoom.set',{magnification:2})).effective.zoomAmount,2);
        const center=await mutation('view.center.set',{point:{space:'scene',x:0.25,y:0.75}});
        assert.equal(center.effective.centerX,0.25);assert.equal(center.effective.centerY,0.75);
        const reset=await mutation('view.reset');assert.equal(reset.effective.zoomAmount,1);
        const state=success(await call('app.state.get'));assert.equal(state.fields.centerX,0.5);
        assert.equal(state.fields.centerY,0.5);success(await call('view.metrics'));
        assert.equal(success(await call('app.wait_for',{condition:'view_presented',timeoutMs:0})).met,true);
    });
    await check('modes reading colors contrast and adjustment operations',async()=>{
        const modes=success(await call('mode.list'));assert.equal(modes.modes[1].presetId,'fixture-document');
        await mutation('mode.activate',{presetId:modes.modes[1].presetId});
        await pause(2100);const colors=await mutation('display.reading_colors.set',{intent:'high_contrast_light_on_dark'});
        assert.equal(colors.effective.scheme,'white-black');
        const contrast=await mutation('image.contrast.set',{contrast:1.234,brightness:0.126});
        assert.equal(contrast.effective.contrast,1.23);assert.equal(contrast.effective.brightness,0.13);
        await pause(2100);const adjust=await mutation('image.auto_adjust',{goal:'read_text',allowModeChange:false});
        assert.equal(adjust.pending,true);assert.equal(adjust.applied.autoContrast,true);
        assert.equal((await operation(adjust.operationId)).state,'requested');await settle();
        assert.equal((await operation(adjust.operationId)).state,'first_frame');
        failure(await call('mode.activate',{presetId:'unknown-mode'}),'not_found');
    });
    await check('argument protocol and standing grant denials',async()=>{
        success(await call('app.state.get',{sinceEvent:Number.MAX_SAFE_INTEGER}));
        failure(await call('app.state.get',{sinceEvent:Number.MAX_SAFE_INTEGER+1}),'invalid_argument');
        failure(await call('view.center.set',{point:{space:'observation',observationId:'fixture',px:1024.01,py:0}}),'invalid_argument');
        failure(await call('view.zoom.set',{magnification:2,anchor:{space:'observation',observationId:'fixture',px:0,py:1e100}}),'invalid_argument');
        failure(await call('view.zoom.set',{magnification:99}),'invalid_argument');
        await assert.rejects(client.listTools({cursor:''}),error=>error.code===-32602);
        await assert.rejects(call('shell.execute'),error=>error.code===-32602);
        failure(await call('view.observe'),'denied_by_policy');
        failure(await call('camera.orientation.set',{degreesClockwise:90}),'denied_by_policy');
        if(hasCapture){failure(await call('capture.photo.take'),'denied_by_policy');
            failure(await call('capture.recording.start',{audio:'none'}),'denied_by_policy');}
        if(hasCamera){const cameras=success(await call('camera.sources.list'));
            failure(await call('camera.select',{cameraHandle:cameras.sources[1].cameraHandle}),'denied_by_policy');}
        const status=await command('fixture-status','fixture-status');assert.equal(status.acceptedPhotos,0);
        assert.equal(status.recordingActive,false);assert.equal(status.cameraHandle,status.savedCameraHandle);
    });
    await check('native user change wins and state refresh',async()=>{
        await command('user-zoom','user-changed');failure(await call('view.zoom.set',{magnification:2}),'stale_state');
        assert.equal(success(await call('app.state.get')).fields.zoomAmount,3);
        assert.equal((await mutation('view.zoom.set',{magnification:2})).effective.zoomAmount,2);
    });
    await check('revocation and explicit frame-only lease',async()=>{
        await command('revoke','revoked');failure(await call('app.state.get'));
        assert.equal((await command(`approve-frames:${peerId}`,'approved')).ok,true);
        const observation=await call('view.observe',{maxLongSide:384});success(observation);
        assert.equal(observation.content.length,2);assert.equal(observation.content[1].mimeType,'image/jpeg');
        assert.equal(observation.structuredContent.jpeg,undefined);assert.equal(observation.structuredContent.data,undefined);
        const image=Buffer.from(observation.content[1].data,'base64');assert.equal(image[0],255);assert.equal(image[1],216);
        assert(image.length<=192*1024);
        if(hasCapture)failure(await call('capture.photo.take'),'denied_by_policy');
    });
    await check('rotation and temporary synthetic camera selection',async()=>{
        await lease('-all');const rotate=await mutation('camera.orientation.set',{degreesClockwise:90});
        assert.equal(rotate.effective.rotation,90);
        if(hasCamera){const cameras=success(await call('camera.sources.list'));assert.equal(cameras.sources.length,2);
            for(const camera of cameras.sources){assert.match(camera.cameraHandle,/^cam-[a-f0-9]{32}$/);assert.match(camera.displayName,/^Camera [12]$/);}
            const before=success(await call('app.state.get'));
            const selected=await mutation('camera.select',{cameraHandle:cameras.sources[1].cameraHandle});
            assert.equal(selected.pending,true);await settle();assert.equal((await operation(selected.operationId)).state,'first_frame');
            const after=success(await call('app.state.get'));assert.equal(after.cameraHandle,cameras.sources[1].cameraHandle);
            assert.equal(after.savedCameraHandle,before.savedCameraHandle);assert(after.versions.sourceEpoch>before.versions.sourceEpoch);
            failure(await call('view.observe'),'denied_by_policy');
            failure(await call('camera.select',{cameraHandle:'cam-'+'0'.repeat(32)}),'not_found');}
    });
    if(hasCapture){
        await check('photo durable pending and synthetic terminal outcome',async()=>{
            await lease('-all');const photo=await mutation('capture.photo.take');assert.equal(photo.pending,true);
            assert.equal((await operation(photo.operationId)).state,'queued');await settle();
            const committed=await operation(photo.operationId);assert.equal(committed.state,'committed');
            assert.equal(committed.originalName,'IMG_fixture_original.jpg');assert.equal(committed.processedName,'IMG_fixture_processed.jpg');
            assert.equal(committed.path,undefined);
        });
        await check('owned recording start stop terminal and source exclusion',async()=>{
            const recording=await mutation('capture.recording.start',{audio:'none'});assert.equal(recording.state,'starting');
            assert.equal(recording.actualAudio,'none');await settle();assert.equal((await operation(recording.operationId)).state,'recording');
            failure(await call('camera.orientation.set',{degreesClockwise:180}),'busy');
            const stopped=await mutation('capture.recording.stop',{sessionId:recording.sessionId});assert.equal(stopped.state,'stopping');
            await settle();assert.equal((await operation(recording.operationId)).state,'completed');
            const again=await mutation('capture.recording.stop',{sessionId:recording.sessionId});assert.equal(again.changed,false);
            assert.equal(again.state,'completed');
        });
        await check('MCP cancellation preserves accepted photo terminal ownership',async()=>{
            await command('hold-photo','photo-held');const controller=new AbortController();
            await pause(550);const promise=client.callTool({name:'capture.photo.take',arguments:{}},undefined,{signal:controller.signal});
            const rejected=assert.rejects(promise);const accepted=await next('photo-accepted');controller.abort();await rejected;
            await client.ping();await command('release-photo','photo-released');await settle();
            assert.equal((await operation(accepted.operationId)).state,'committed');
            const state=success(await call('app.state.get'));assert.equal(state.acceptedPhotos,2);
        });
        await check('revoke pending photo suppresses old lease result but retains accepted work',async()=>{
            await command('hold-photo','photo-held');await pause(550);
            const pending=call('capture.photo.take');const accepted=await next('photo-accepted');
            await command('revoke','revoked');failure(await pending);
            await command('release-photo','photo-released');await settle();
            assert.equal((await command(`approve-all:${peerId}`,'approved')).ok,true);
            assert.equal((await operation(accepted.operationId)).state,'committed');
            assert.equal(success(await call('app.state.get')).acceptedPhotos,3);
        });
        await check('lease revoke stops only exact owned recording and retains outcome',async()=>{
            const recording=await mutation('capture.recording.start',{audio:'saved_microphone'});
            assert.equal(recording.actualAudio,'none');await settle();await command('revoke','revoked');
            const status=await command('fixture-status','fixture-status');assert.equal(status.recordingActive,false);
            assert.equal(status.ownedStops,2);failure(await call('capture.recording.stop',{sessionId:recording.sessionId}));
            assert.equal((await command(`approve-all:${peerId}`,'approved')).ok,true);await settle();
            assert.equal((await operation(recording.operationId)).state,'completed');
            failure(await call('capture.recording.stop',{sessionId:recording.sessionId}),'denied_by_policy');
        });
        await check('manual recording never adopted or stopped by standing grant',async()=>{
            const manual=await command('manual-recording','manual-started');
            failure(await call('capture.recording.stop',{sessionId:manual.sessionId}),'denied_by_policy');
            failure(await call('capture.recording.start',{audio:'none'}),'already_active');
            await command('revoke','revoked');const status=await command('fixture-status','fixture-status');
            assert.equal(status.recordingActive,true);assert.equal(status.sessionId,manual.sessionId);assert.equal(status.ownerKind,'manual');
            assert.equal(status.ownedStops,2);
        });
    }
    await check('normal EOF and private app cleanup',async()=>{
        await client.close();client=null;app.stdin.end('exit\n');
        const exit=await Promise.race([appExit,new Promise((_,reject)=>exitTimer=setTimeout(()=>reject(new Error('fixture exit timeout')),8000))]);
        clearTimeout(exitTimer);assert.equal(exit.code,0);assert.equal(exit.signal,null);assert.equal(appErrors,'');
    });
    console.log(`PASS: SDK 1.32.1 ${evidence.length} independent-client scenarios (${edition}); synthetic target only.`);
} finally {
    clearTimeout(exitTimer);
    if(client)await client.close().catch(()=>{});
    if(app.exitCode===null)app.stdin.end('exit\n');
    lines.close();
    if(args.get('--evidence-dir')){
        const folder=resolve(args.get('--evidence-dir'));const local=resolve(process.env.LOCALAPPDATA);
        assert(folder.toLowerCase().startsWith(local.toLowerCase()+'\\'),'Evidence must stay in LocalAppData');
        await mkdir(folder,{recursive:true});
        const escape=text=>String(text).replace(/[<>&"']/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;',"'":'&apos;'}[c]));
        await writeFile(join(folder,'mcp-sdk-evidence.json'),JSON.stringify({sdk:'1.32.1',catalog:edition,
            synthetic:true,hardwareAcceptance:false,scenarios:evidence},null,2)+'\n');
        await writeFile(join(folder,'mcp-sdk-junit.xml'),`<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="MCP independent SDK private fixture" tests="${evidence.length}" failures="${evidence.filter(x=>x.status==='failed').length}">${evidence.map(x=>`<testcase name="${escape(x.name)}" time="${x.durationMs/1000}">${x.status==='failed'?`<failure message="${escape(x.error)}"/>`:''}</testcase>`).join('')}</testsuite>\n`);
    }
}
