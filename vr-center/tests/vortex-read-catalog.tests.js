'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'vortex-catalog-'));
const data = path.join(root, 'data'), game = path.join(root, 'game');
const staging = path.join(data, 'cyberpunk2077', 'mods');
function put(mod, name, value) { const p = path.join(staging, mod, name); fs.mkdirSync(path.dirname(p), {recursive:true}); fs.writeFileSync(p, value); }
put('A', 'archive/pc/mod/shared.archive', 'A');
put('B', 'archive/pc/mod/shared.archive', 'B');
put('A', 'config/custom.json', '{"user":true}');
put('A', '__vortex_staging_folder', 'marker');
put('A', '__folder_managed_by_vortex', 'marker');
const fixture = {settings:{profiles:{activeProfileId:'one'}}, persistent:{deployment:{needToDeploy:{cyberpunk2077:false}}, profiles:{
  one:{gameId:'cyberpunk2077',name:'First',modState:{A:{enabled:true},B:{enabled:true},collection:{enabled:true}}},
  missing:{gameId:'cyberpunk2077',name:'Missing',modState:{gone:{enabled:true}}},
  wrong:{gameId:'cyberpunk2077',name:'Traversal',modState:{wrong:{enabled:true}}},
  anotherGame:{gameId:'skyrim',name:'Ignore',modState:{}}
},mods:{cyberpunk2077:{
  A:{state:'installed',installationPath:'A',rules:[{type:'before',reference:{id:'B'}}]},
  B:{state:'installed',installationPath:'B'},collection:{state:'installed',type:'collection'},
  gone:{state:'installed',installationPath:'Gone'},wrong:{state:'installed',installationPath:'../outside'}
}}}};
const {readCatalog,allowedKey} = require('../integrations/vortex/read-catalog.js');
const before = JSON.stringify(fixture);
let catalog=readCatalog(fixture,{data,game});
assert.equal(catalog.profiles.length,3);
let first=catalog.profiles.find(p=>p.id==='one');
assert(first.available);assert.equal(first.enabledCount,2);
assert.equal(first.files.find(f=>f.relative.endsWith('shared.archive')).modId,'B');
assert(first.files.some(f=>f.relative==='config/custom.json'));
assert(!first.files.some(f=>f.relative.includes('__vortex')));
assert(!first.files.some(f=>f.relative.includes('__folder_managed')));
assert(!catalog.profiles.find(p=>p.id==='missing').available);
assert.match(catalog.profiles.find(p=>p.id==='wrong').reason,/path|путь|outside/i);
assert.equal(JSON.stringify(fixture),before,'read order and original state must not be mutated');
assert(allowedKey('persistent###profiles###one###modState###A###enabled'));
assert(allowedKey('persistent###mods###cyberpunk2077###A###rules'));
assert(!allowedKey('persistent###mods###cyberpunk2077###A###attributes###author'));
assert(!allowedKey('confidential###account###APIKey'));
assert(!allowedKey('persistent###downloads###anything'));
const flat={};
function flatten(value,prefix='') {for(const [key,item] of Object.entries(value)){const name=prefix?prefix+'###'+key:key;if(item && typeof item==='object' && !Array.isArray(item))flatten(item,name);else flat[name]=item;}}
flatten(fixture);
assert.deepStrictEqual(readCatalog(flat,{data,game}),catalog,'leaf-key snapshot hierarchy gives the same inventory and precedence');
fixture.persistent.mods.cyberpunk2077.A.rules=[];
catalog=readCatalog(fixture,{data,game});
assert(!catalog.profiles.find(p=>p.id==='one').available,'different content without precedence must fail');
fixture.deployment={version:1,deploymentMethod:'hardlink_activator',gameId:'cyberpunk2077',targetPath:game,stagingPath:staging,files:[{relPath:'archive\\pc\\mod\\shared.archive',source:'A',target:'',time:1},{relPath:'config/custom.json',source:'A',target:'',time:1}]};
catalog=readCatalog(fixture,{data,game});
first=catalog.profiles.find(p=>p.id==='one');assert(first.available);
assert.equal(first.files.find(f=>f.relative.endsWith('shared.archive')).modId,'A','proven deployed winner takes priority');
fixture.persistent.deployment.needToDeploy.cyberpunk2077=true;
assert(!readCatalog(fixture,{data,game}).profiles.find(p=>p.id==='one').available,'pending deployment cannot prove a winner');
fixture.persistent.mods.cyberpunk2077.A.fileOverrides=['archive/pc/mod/shared.archive'];
assert.equal(readCatalog(fixture,{data,game}).profiles.find(p=>p.id==='one').files.find(f=>f.relative.endsWith('shared.archive')).modId,'A');
fixture.persistent.mods.cyberpunk2077.A.fileOverrides=[];
put('B','archive/pc/mod/shared.archive','A');
assert(readCatalog(fixture,{data,game}).profiles.find(p=>p.id==='one').available,'identical duplicate content is safe');
fs.writeFileSync(path.join(root,'input.json'),JSON.stringify(fixture));
const spawned=require('child_process').spawnSync(process.execPath,[path.join(__dirname,'../integrations/vortex/read-catalog.js'),'--fixture',path.join(root,'input.json'),'--data',data,'--game',game,'--output',path.join(root,'catalog.json')],{encoding:'utf8'});
assert.equal(spawned.status,0,spawned.stderr);
assert.equal(JSON.parse(fs.readFileSync(path.join(root,'catalog.json'),'utf8')).profiles.length,3);
console.log('PASS: Vortex catalogue missing/path/rules/deployment/override/duplicate/read-only fixtures');
console.log('Fixture: '+root);
if(process.env.ELECTRON_RUN_AS_NODE==='1') {
  (async()=>{
    const leveldown=require(path.join(path.dirname(process.execPath),'resources/app.asar/node_modules/leveldown'));
    const snapshot=path.join(root,'db-snapshot'),db=leveldown(snapshot);
    await new Promise((resolve,reject)=>db.open({createIfMissing:true},e=>e?reject(e):resolve()));
    const operations=Object.entries(flat).filter(([key])=>allowedKey(key)).map(([key,value])=>({type:'put',key,value:JSON.stringify(value)}));
    operations.push({type:'put',key:'persistent###mods###cyberpunk2077###A###attributes###author',value:'not-json-must-never-be-read'});
    operations.push({type:'put',key:'confidential###account###APIKey',value:'not-json-must-never-be-read'});
    await new Promise((resolve,reject)=>db.batch(operations,e=>e?reject(e):resolve()));
    await new Promise((resolve,reject)=>db.close(e=>e?reject(e):resolve()));
    const run=require('child_process').spawnSync(process.execPath,[path.join(__dirname,'../integrations/vortex/read-catalog.js'),'--db',snapshot,'--data',data,'--game',game,'--output',path.join(root,'db-catalog.json')],{encoding:'utf8'});
    assert.equal(run.status,0,run.stderr);
    const result=JSON.parse(fs.readFileSync(path.join(root,'db-catalog.json'),'utf8'));
    assert.equal(result.activeProfileId,'one');assert(result.profiles.find(p=>p.id==='one').available);
    assert(!JSON.stringify(result).includes('must-never-be-read'));
    console.log('PASS: actual Vortex Electron/leveldown snapshot CLI and value allowlist');
  })().catch(error=>{console.error(error);process.exitCode=1;});
}
