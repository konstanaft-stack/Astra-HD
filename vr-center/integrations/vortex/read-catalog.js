'use strict';
// Reads a caller-created LevelDB snapshot. Never opens Vortex's live state database.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const GAME = 'cyberpunk2077';
const fold = s => String(s).toLowerCase();
const samePath = (a,b) => fold(path.resolve(a)) === fold(path.resolve(b));
function inside(root,p) { const r=path.relative(path.resolve(root),path.resolve(p)); return r==='' || (!r.startsWith('..'+path.sep) && r!=='..' && !path.isAbsolute(r)); }
function relativeName(value) {
  if(typeof value!=='string' || !value || /[\0:]/.test(value) || path.win32.isAbsolute(value) || path.posix.isAbsolute(value)) throw Error('Unsafe relative path: '+String(value));
  const parts=value.replace(/\\/g,'/').split('/');
  if(parts.some(p=>!p || p==='.' || p==='..' || /[. ]$/.test(p))) throw Error('Unsafe relative path: '+value);
  return parts.join('/');
}
function noLinks(p) {
  const full=path.resolve(p), parsed=path.parse(full);let at=parsed.root;
  for(const part of full.slice(parsed.root.length).split(path.sep).filter(Boolean)) {
    at=path.join(at,part);
    if(fs.lstatSync(at).isSymbolicLink()) throw Error('Reparse/symbolic path is not supported: '+at);
  }
}
function child(root,rel) { const result=path.resolve(root,...relativeName(rel).split('/'));if(!inside(root,result))throw Error('Source outside staging path');return result; }
function assign(tree,parts,value) {
  let at=tree;
  for(const part of parts.slice(0,-1)) {if(['__proto__','prototype','constructor'].includes(part))throw Error('Invalid state key');if(!at[part] || typeof at[part]!=='object')at[part]={};at=at[part];}
  const tail=parts[parts.length-1];if(['__proto__','prototype','constructor'].includes(tail))throw Error('Invalid state key');at[tail]=value;
}
function expand(obj) {
  if(!obj || typeof obj!=='object' || Array.isArray(obj))return obj;
  const result={};for(const [key,value] of Object.entries(obj))assign(result,key.split('###'),expand(value));return result;
}
function filesIn(root) {
  noLinks(root);if(!fs.statSync(root).isDirectory())throw Error('Missing installed mod directory: '+root);
  const output=[];
  function visit(dir,prefix) {
    for(const entry of fs.readdirSync(dir,{withFileTypes:true}).sort((a,b)=>a.name.localeCompare(b.name))) {
      // Vortex's staging markers are metadata, not game files. No other files are silently filtered.
      if(['__vortex_staging_folder','__folder_managed_by_vortex'].includes(fold(entry.name)))continue;
      const rel=relativeName(prefix+entry.name),full=child(root,rel);noLinks(full);
      if(entry.isDirectory())visit(full,rel+'/');
      else if(entry.isFile()) {const stat=fs.statSync(full);output.push({relative:rel,source:full,size:stat.size,lastWriteUtc:stat.mtime.toISOString()});}
      else throw Error('Unsupported staging entry: '+full);
    }
  }
  visit(root,'');return output;
}
function referenceMatches(mod,ref) {
  if(!ref || typeof ref!=='object')return false;
  const a=mod.attributes||{};let selectors=0;
  for(const [key,actual] of [['id',mod.id],['modId',a.modId],['fileId',a.fileId],['fileMD5',a.fileMD5],['logicalFileName',a.logicalFileName]]) {
    if(ref[key]!==undefined) {selectors++;if(fold(ref[key])!==fold(actual))return false;}
  }
  if(ref.fileExpression) {
    selectors++;const pattern='^'+String(ref.fileExpression).replace(/[.+^${}()|[\]\\]/g,'\\$&').replace(/\*/g,'.*').replace(/\?/g,'.')+'$';
    if(!new RegExp(pattern,'i').test(String(a.logicalFileName||mod.id)))return false;
  }
  if(ref.versionMatch && ref.versionMatch!=='*' && ref.versionMatch!==a.version && ref.versionMatch!=='='+a.version)return false;
  return selectors>0;
}
function graphFor(mods) {
  const graph=new Map(mods.map(m=>[m.id,new Set()]));
  for(const mod of mods)for(const rule of mod.rules||[]) {
    if(!['before','after','requires'].includes(rule.type))continue;
    const matches=mods.filter(m=>referenceMatches(m,rule.reference));
    if(rule.type==='requires' && matches.length!==1)throw Error('Unresolved required mod for '+mod.id);
    if(matches.length>1)throw Error('Ambiguous mod rule for '+mod.id);
    if(matches.length===1 && matches[0].id!==mod.id) {
      if(rule.type==='before')graph.get(mod.id).add(matches[0].id);
      else graph.get(matches[0].id).add(mod.id);
    }
  }
  const visiting=new Set(),done=new Set();
  function check(id) {if(visiting.has(id))throw Error('Cyclic ordering rules: '+id);if(done.has(id))return;visiting.add(id);for(const next of graph.get(id))check(next);visiting.delete(id);done.add(id);}
  for(const id of graph.keys())check(id);
  return graph;
}
function follows(graph,from,to,seen=new Set()) {if(seen.has(from))return false;seen.add(from);for(const next of graph.get(from)||[])if(next===to || follows(graph,next,to,seen))return true;return false;}
function hashFile(p,cache) {if(!cache.has(p))cache.set(p,crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex'));return cache.get(p);}
function provenDeployment(state,profile,mods,candidates,staging,game) {
  const manifest=state.deployment;
  if(!manifest || profile.id!==state.settings?.profiles?.activeProfileId || state.persistent?.deployment?.needToDeploy?.[GAME]!==false)return null;
  if(manifest.profileId && manifest.profileId!==profile.id)return null;
  if(manifest.gameId!==GAME || manifest.version!==1 || manifest.deploymentMethod!=='hardlink_activator' || !manifest.stagingPath || !manifest.targetPath || !samePath(manifest.stagingPath,staging) || !samePath(manifest.targetPath,game) || !Array.isArray(manifest.files))return null;
  const winners=new Map();
  try {
    for(const file of manifest.files) {
      if(file.target) return null;
      const key=fold(relativeName(file.relPath)),source=relativeName(file.source);
      const owners=mods.filter(m=>fold(relativeName(m.installationPath))===fold(source));
      if(owners.length!==1 || winners.has(key))return null;
      const match=(candidates.get(key)||[]).find(c=>c.modId===owners[0].id);
      if(!match)return null;
      winners.set(key,match);
    }
    // Actual deployed manifest has no profileId. Full source coverage and enabled
    // ownership, matching paths and needToDeploy=false bind it to the active state.
    if(winners.size!==candidates.size)return null;
    return winners;
  } catch(_) {return null;}
}
function readCatalog(input,options) {
  const state=expand(input),data=path.resolve(options.data),game=path.resolve(options.game);
  const staging=path.resolve(state.stagingPath || state.settings?.mods?.installPath?.[GAME] || path.join(data,GAME,'mods'));
  const rawMods=state.persistent?.mods?.[GAME]||{},activeProfileId=state.settings?.profiles?.activeProfileId||null;
  const result={schema:1,root:data,stagingPath:staging,activeProfileId,profiles:[]},hashes=new Map();
  for(const [id,raw] of Object.entries(state.persistent?.profiles||{})) {
    if(raw.gameId!==GAME)continue;
    const profile={id,name:raw.name||id,enabledCount:0,active:id===activeProfileId,available:true,reason:'',files:[],modIds:[]};result.profiles.push(profile);
    profile.modIds=Object.entries(raw.modState||{}).filter(([modId,choice])=>choice.enabled===true && rawMods[modId]?.type!=='collection').map(([modId])=>modId);
    profile.enabledCount=profile.modIds.length;
    try {
      noLinks(staging);
      const mods=[];
      for(const modId of profile.modIds) {
        const mod=rawMods[modId];
        if(!mod || mod.state!=='installed' || !mod.installationPath)throw Error('Enabled mod is not installed: '+modId);
        mods.push({...mod,id:modId});
      }
      const candidates=new Map();
      for(const mod of mods) {
        const root=child(staging,mod.installationPath);
        if(!fs.existsSync(root))throw Error('Missing installed mod: '+mod.id+' ('+root+')');
        for(const file of filesIn(root)) {const key=fold(file.relative);if(!candidates.has(key))candidates.set(key,[]);candidates.get(key).push({...file,modId:mod.id});}
      }
      const deployed=provenDeployment(state,{id},mods,candidates,staging,game),graph=deployed ? new Map() : graphFor(mods);
      for(const [key,choices] of candidates) {
        let winner=choices.length===1 ? choices[0] : deployed?.get(key);
        if(!winner) {
          const override=choices.filter(c=>(mods.find(m=>m.id===c.modId).fileOverrides||[]).some(p=>fold(relativeName(p))===key));
          const pool=override.length ? override : choices;
          const latest=pool.filter(c=>!pool.some(other=>other!==c && follows(graph,c.modId,other.modId)));
          if(latest.length===1)winner=latest[0];
          else if(new Set(latest.map(c=>hashFile(c.source,hashes))).size===1)winner=latest[0];
          else throw Error('Ambiguous different-content file: '+choices[0].relative+' ('+latest.map(c=>c.modId).join(', ')+')');
        }
        profile.files.push(winner);
      }
      profile.files.sort((a,b)=>fold(a.relative).localeCompare(fold(b.relative)));
    } catch(error) {profile.available=false;profile.reason=error.message;profile.files=[];}
  }
  return result;
}
function allowedKey(key) {
  const p=key.split('###');
  if(p[0]==='settings')return (p[1]==='profiles' && p[2]==='activeProfileId') || (p[1]==='mods' && p[2]==='installPath' && p[3]===GAME);
  if(p[0]!=='persistent')return false;
  if(p[1]==='deployment')return p[2]==='needToDeploy' && p[3]===GAME;
  if(p[1]==='profiles')return ['gameId','name','id'].includes(p[3]) || (p[3]==='modState' && p[5]==='enabled');
  if(p[1]==='mods' && p[2]===GAME)return ['state','installationPath','type','rules','fileOverrides'].includes(p[4]) || (p[4]==='attributes' && ['modId','fileId','name','version','fileMD5','logicalFileName'].includes(p[5]));
  return false;
}
async function readSnapshot(dbPath,data) {
  if(inside(data,dbPath))throw Error('Refusing live Vortex data; provide a snapshot outside the Vortex data directory');
  noLinks(dbPath);
  const leveldown=require(path.join(path.dirname(process.execPath),'resources','app.asar','node_modules','leveldown'));
  const db=leveldown(dbPath);
  await new Promise((resolve,reject)=>db.open({createIfMissing:false},e=>e?reject(e):resolve()));
  const state={};
  const prefixes=['settings###profiles###activeProfileId','settings###mods###installPath###'+GAME,'persistent###deployment###needToDeploy###'+GAME,'persistent###profiles###','persistent###mods###'+GAME+'###'];
  try {
    for(const prefix of prefixes) {
      // Enumerate keys only. Values are fetched separately only after the exact
      // metadata allowlist; credentials, download URLs and authors are never read.
      const iterator=db.iterator({gte:prefix,lt:prefix+'\uffff',keys:true,values:false,keyAsBuffer:false});
      try {
        while(true) {
          const key=await new Promise((resolve,reject)=>iterator.next((e,k)=>e?reject(e):resolve(k)));
          if(key===undefined)break;
          if(!allowedKey(String(key)))continue;
          const value=await new Promise((resolve,reject)=>db.get(key,{asBuffer:false},(e,v)=>e?reject(e):resolve(v)));
          assign(state,String(key).split('###'),JSON.parse(String(value)));
        }
      } finally {await new Promise(r=>iterator.end(r));}
    }
  } finally {await new Promise(r=>db.close(r));}
  return state;
}
async function main() {
  const args={};for(let i=2;i<process.argv.length;i+=2){if(!process.argv[i].startsWith('--') || !process.argv[i+1])throw Error('Expected --name value');args[process.argv[i].slice(2)]=process.argv[i+1];}
  if(!args.data || !args.game || !args.output || (!args.fixture && !args.db))throw Error('Required: --db snapshot (or --fixture JSON) --data root --game path --output JSON');
  const state=args.fixture ? JSON.parse(fs.readFileSync(args.fixture,'utf8').replace(/^\uFEFF/,'')) : await readSnapshot(path.resolve(args.db),path.resolve(args.data));
  if(!args.fixture) {
    const manifest=path.join(args.game,'vortex.deployment.json');
    if(fs.existsSync(manifest)){noLinks(manifest);state.deployment=JSON.parse(fs.readFileSync(manifest,'utf8').replace(/^\uFEFF/,''));}
  }
  const catalog=readCatalog(state,{data:args.data,game:args.game});
  for(const root of [args.db,args.data,args.game,catalog.stagingPath].filter(Boolean))if(inside(root,args.output))throw Error('Output must be outside snapshot, game and Vortex data');
  fs.mkdirSync(path.dirname(path.resolve(args.output)),{recursive:true});fs.writeFileSync(args.output,JSON.stringify(catalog,null,2),'utf8');
}
module.exports={readCatalog,allowedKey};
if(require.main===module)main().catch(e=>{process.stderr.write(e.message+'\n');process.exitCode=1;});
