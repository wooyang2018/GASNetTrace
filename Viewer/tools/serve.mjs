import { createServer } from 'node:http'
import { randomBytes, timingSafeEqual } from 'node:crypto'
import { readFile, stat } from 'node:fs/promises'
import { extname, join, normalize } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = join(fileURLToPath(new URL('..', import.meta.url)), 'dist')
// Pass the commandlet's printed token here to use one credential for both the
// static viewer and its Analyzer API requests.
const token = process.env.GAS_NET_TRACE_TOKEN || randomBytes(24).toString('base64url')
const port = Number(process.env.GAS_NET_TRACE_VIEWER_PORT || 4173)
const mime = { '.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8','.json':'application/json' }
const tokenMatches = value => {
  const a=Buffer.from(value||''), b=Buffer.from(token)
  return a.length===b.length && timingSafeEqual(a,b)
}
const cookieToken = header => {
  const entry=(header||'').split(';').map(part=>part.trim()).find(part=>part.startsWith('gas_net_trace_token='))
  if(!entry)return ''
  try{return decodeURIComponent(entry.slice('gas_net_trace_token='.length))}catch{return ''}
}

createServer(async(req,res)=>{
  const url=new URL(req.url||'/',`http://127.0.0.1:${port}`)
  const queryToken=url.searchParams.get('token')
  if(!tokenMatches(queryToken||req.headers['x-gas-net-trace-token']||cookieToken(req.headers.cookie))){res.writeHead(401,{'content-type':'text/plain'});res.end('Invalid GAS Net Trace access token');return}
  if(queryToken&&tokenMatches(queryToken))res.setHeader('set-cookie',`gas_net_trace_token=${encodeURIComponent(token)}; Path=/; HttpOnly; SameSite=Strict`)
  if(url.pathname==='/api/health'){res.writeHead(200,{'content-type':'application/json'});res.end(JSON.stringify({ok:true,loopback:true,schema:'gasnettrace.service.v2'}));return}
  let relative=url.pathname==='/'?'index.html':url.pathname.slice(1), path=normalize(join(root,relative))
  if(!path.startsWith(root)){res.writeHead(403);res.end();return}
  try{if((await stat(path)).isDirectory())path=join(path,'index.html');const body=await readFile(path);res.writeHead(200,{'content-type':mime[extname(path)]||'application/octet-stream','cache-control':'no-store'});res.end(body)}catch{const body=await readFile(join(root,'index.html'));res.writeHead(200,{'content-type':'text/html; charset=utf-8'});res.end(body)}
}).listen(port,'127.0.0.1',()=>console.log(`GAS Net Trace Viewer: http://127.0.0.1:${port}/?token=${token}`))
