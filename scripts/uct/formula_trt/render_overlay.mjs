/** Offline formula-result rendering. Uses actual tokens, never an equation template. */
import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {createHash} from 'node:crypto';
import {performance} from 'node:perf_hooks';

export function validateRegion(region,width,height) {
  const q=region.sourceLTRB;
  if (!Array.isArray(q)||q.length!==4||q.some(v=>!Number.isSafeInteger(v))||q[0]<0||q[1]<0||q[2]>width||q[3]>height||q[2]<=q[0]||q[3]<=q[1]) throw Error('invalid source region');
  if (typeof region.latex!=='string'||!region.latex.trim()||region.latex.length>8192) throw Error('invalid formula');
  if (/\\(?:require|href|url|includegraphics|html|class|style|def|newcommand|renewcommand)\b/.test(region.latex)) throw Error('unsupported formula command');
  return {left:q[0],top:q[1],width:q[2]-q[0],height:q[3]-q[1]};
}

export function makeRenderer(dependencyRoot) {
  const req=createRequire(path.join(path.resolve(dependencyRoot),'package.json'));
  const {mathjax}=req('mathjax-full/js/mathjax.js');
  const {TeX}=req('mathjax-full/js/input/tex.js');
  const {SVG}=req('mathjax-full/js/output/svg.js');
  const {liteAdaptor}=req('mathjax-full/js/adaptors/liteAdaptor.js');
  const {RegisterHTMLHandler}=req('mathjax-full/js/handlers/html.js');
  req('mathjax-full/js/input/tex/base/BaseConfiguration.js');
  req('mathjax-full/js/input/tex/ams/AmsConfiguration.js');
  const adaptor=liteAdaptor();RegisterHTMLHandler(adaptor);
  const document=mathjax.document('',{InputJax:new TeX({packages:['base','ams'],maxBuffer:8192}),OutputJax:new SVG({fontCache:'none'})});
  const cache=new Map();
  function typeset(latex) {
    const started=performance.now();
    if (cache.has(latex)) return {...cache.get(latex),cacheHit:true,layoutMs:performance.now()-started};
    const container=document.convert(latex,{display:true});
    const svg=adaptor.outerHTML(adaptor.firstChild(container));
    if (/data-mjx-error|<merror|data-mml-node="merror"/.test(svg)) throw Error('formula typesetting failed');
    const v=svg.match(/viewBox="([^"]+)"/);
    if (!v) throw Error('missing vector geometry');
    const viewBox=v[1].trim().split(/\s+/).map(Number);
    if (viewBox.length!==4||viewBox.some(v=>!Number.isFinite(v))||viewBox[2]<=0||viewBox[3]<=0) throw Error('invalid vector geometry');
    const content=svg.slice(svg.indexOf('>')+1,svg.lastIndexOf('</svg>'));
    const result={content,viewBox,cacheHit:false,layoutMs:performance.now()-started};
    if (cache.size>=64) cache.delete(cache.keys().next().value);
    cache.set(latex,result);return result;
  }
  return {typeset,sharp:req('sharp'),version:req('mathjax-full/package.json').version,sharpVersion:req('sharp/package.json').version};
}

const hash=buf=>createHash('sha256').update(buf).digest('hex');
export async function render(requestFile,outputDirectory,dependencyRoot) {
  if (fs.existsSync(outputDirectory)) throw Error('preserve prior output directory');
  const requestBytes=fs.readFileSync(requestFile),request=JSON.parse(requestBytes);
  if (request.version!==1||!Array.isArray(request.regions)||request.regions.length>8) throw Error('invalid render request');
  const source=fs.readFileSync(request.source);
  if (hash(source)!==request.sourceSha256||hash(fs.readFileSync(request.prediction))!==request.predictionSha256) throw Error('source/prediction changed');
  const boot=performance.now(),renderer=makeRenderer(dependencyRoot),setupMs=performance.now()-boot;
  const {sharp}=renderer, metadata=await sharp(source).metadata();
  if (![3,4].includes(metadata.channels)) throw Error('RGB/RGBA source required');
  if (metadata.width*metadata.height>67108864) throw Error('image pixel bound');
  const regions=request.regions.map(r=>({...r,box:validateRegion(r,metadata.width,metadata.height)}));
  fs.mkdirSync(outputDirectory,{recursive:true});
  const results=[];
  for (const [name,color] of [['white','#ffffff'],['yellow','#ffff00']]) {
    const plates=[],entries=[];const started=performance.now();
    for (const region of regions) {
      const {box}=region,vector=renderer.typeset(region.latex);
      const inset=3,w=Math.max(1,box.width-2*inset),h=Math.max(1,box.height-2*inset);
      const plate=`<svg xmlns="http://www.w3.org/2000/svg" width="${box.width}" height="${box.height}" viewBox="0 0 ${box.width} ${box.height}" style="color:${color}"><rect width="100%" height="100%" fill="#000000"/><svg x="${inset}" y="${inset}" width="${w}" height="${h}" viewBox="${vector.viewBox.join(' ')}" preserveAspectRatio="xMidYMid meet">${vector.content}</svg></svg>`;
      const svgName=`${name}-${entries.length}.svg`;fs.writeFileSync(path.join(outputDirectory,svgName),plate);
      plates.push({input:Buffer.from(plate),left:box.left,top:box.top});
      entries.push({case:region.case,latex:region.latex,sourceLTRB:region.sourceLTRB,layoutMs:vector.layoutMs,cacheHit:vector.cacheHit,svg:svgName});
    }
    const png=await sharp(source).composite(plates).png().toBuffer();
    const compositeMs=performance.now()-started;
    // Verify that neither the diagram nor any pixels outside the regions changed.
    const original=await sharp(source).removeAlpha().raw().toBuffer();
    const actual=await sharp(png).removeAlpha().raw().toBuffer();
    let outsideChanged=0,insideChanged=0;
    for(let y=0;y<metadata.height;y++) for(let x=0;x<metadata.width;x++) {
      const inside=regions.some(r=>x>=r.box.left&&x<r.box.left+r.box.width&&y>=r.box.top&&y<r.box.top+r.box.height);
      const i=(y*metadata.width+x)*3,changed=actual[i]!==original[i]||actual[i+1]!==original[i+1]||actual[i+2]!==original[i+2];
      if (changed) {if(inside)insideChanged++;else outsideChanged++;}
    }
    if (outsideChanged||!insideChanged) throw Error('overlay pixel ownership failed');
    fs.writeFileSync(path.join(outputDirectory,name+'.png'),png);
    results.push({palette:name,pngSha256:hash(png),compositeMs,outsideChangedPixels:outsideChanged,insideChangedPixels:insideChanged,regions:entries});
  }
  const cacheTimings=[];
  for(let i=0;i<50;i++) for(const r of regions) cacheTimings.push(renderer.typeset(r.latex).layoutMs);
  cacheTimings.sort((a,b)=>a-b);
  const receipt={version:1,requestSha256:hash(requestBytes),sourceSha256:request.sourceSha256,predictionSha256:request.predictionSha256,model:request.model,precision:request.precision,renderer:{mathjax:renderer.version,sharp:renderer.sharpVersion,setupMs},cachedSvgMedianMs:cacheTimings[Math.floor(cacheTimings.length/2)],placement:request.placement,results};
  fs.writeFileSync(path.join(outputDirectory,'render.json'),JSON.stringify(receipt,null,2));
  return receipt;
}

if(process.argv[1]&&path.resolve(process.argv[1])===path.resolve(new URL(import.meta.url).pathname.replace(/^\/([A-Z]:)/,'$1'))) {
  const [request,output,deps]=process.argv.slice(2);
  if(!request||!output||!deps) throw Error('usage: node render_overlay.mjs REQUEST OUTPUT_DIRECTORY DEPENDENCY_ROOT');
  console.log(JSON.stringify(await render(request,output,deps),null,2));
}
