// Check the Native binding registry against the compiler's actual C call graph.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

const root=path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const build=path.resolve(process.argv[2]);
const commands=JSON.parse(fs.readFileSync(path.join(build,'compile_commands.json'),'utf8'));
const scratch=fs.mkdtempSync(path.join(os.tmpdir(),'laplace-bindings-'));
const graph=new Map(), globals=new Map(), exports=new Map();
function callsOf(node,calls){
  if(node.kind==='CallExpr'){
    const pending=[node.inner?.[0]];
    while(pending.length){const x=pending.pop();if(!x)continue;
      if(x.kind==='DeclRefExpr'&&x.referencedDecl?.kind==='FunctionDecl')calls.add(x.referencedDecl.name);
      else pending.push(...(x.inner||[]));
    }
  }
  for(const child of node.inner||[])callsOf(child,calls);
}
try{
  for(const command of commands.filter(c=>path.dirname(path.resolve(c.directory,c.file))===path.join(root,'src'))){
    const args=command.arguments||command.command.match(/"(?:[^"\\]|\\.)*"|'[^']*'|\S+/g)
      .map(s=>(s[0]==='"'||s[0]==="'")?s.slice(1,-1):s);
    const argv=[];
    const msvc=process.platform==='win32' && args.some(a=>/^[/\-]Fo/.test(a));
    for(let i=1;i<args.length;i++){
      if(args[i]==='-o' || (msvc && /^[/\-]F[od]$/.test(args[i]))){i++;continue;}
      if(args[i]==='-c' || args[i]==='/c' || (msvc && /^[/\-]F[od]/.test(args[i])))continue;
      argv.push(args[i]);
    }
    argv.push(...(msvc?['/clang:-fsyntax-only','/clang:-Xclang','/clang:-ast-dump=json']:['-fsyntax-only','-Xclang','-ast-dump=json']));
    const output=path.join(scratch,'ast.json'),fd=fs.openSync(output,'w');
    let result;try{result=spawnSync(args[0],argv,{cwd:command.directory,stdio:['ignore',fd,'inherit']});}finally{fs.closeSync(fd);}
    if(result.error || result.status!==0)throw Error(`Compiler call-graph extraction failed for ${command.file}: ${result.error || result.status}`);
    const ast=JSON.parse(fs.readFileSync(output,'utf8'));
    for(const node of ast.inner||[])if(node.kind==='FunctionDecl'){
      if(node.name.startsWith('pg_finfo_'))exports.set(node.name.slice(9),command.file);
      const body=node.inner?.find(x=>x.kind==='CompoundStmt');if(!body)continue;
      const calls=new Set();callsOf(body,calls);
      const key=command.file+':'+node.name;
      graph.set(key,{unit:command.file,calls});if(node.storageClass!=='static')globals.set(node.name,key);
    }
    fs.unlinkSync(output);
  }
  const expected=new Map(fs.readFileSync(path.join(root,'manifest/function_bindings.tsv'),'utf8').split('\n')
    .filter(l=>l&&!l.startsWith('#')).map(l=>{const [name,scope]=l.split(/\s+/);return[name,scope==='true'];}));
  if(!exports.size)throw Error('Compiler reported no Native exports');
  for(const [name,unit] of exports){
    const seen=new Set(),todo=[unit+':'+name];let sql=false;
    while(todo.length){const key=todo.pop();if(seen.has(key))continue;seen.add(key);
      const node=graph.get(key);if(!node)continue;
      for(const call of node.calls){if(call.startsWith('SPI_'))sql=true;
        const local=node.unit+':'+call;const target=graph.has(local)?local:globals.get(call);if(target)todo.push(target);
      }
    }
    if(!expected.has(name)||expected.get(name)!==sql)throw Error(`Binding registry disagrees with Native call graph: ${name}, sql_scope=${sql}`);
    expected.delete(name);
  }
  if(expected.size)throw Error(`Binding registry contains missing Native exports: ${[...expected.keys()].join(', ')}`);
  console.log(`Native function bindings: ${exports.size} exports match the compiler call graph`);
}finally{fs.rmSync(scratch,{recursive:true,force:true});}
