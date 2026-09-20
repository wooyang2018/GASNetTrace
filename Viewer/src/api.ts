import type{CausalityChain,Diagnostic,Metrics,NetworkEvidence,Page,TraceEvent,Workspace}from'./types'

const params=new URLSearchParams(location.search)
const base=(params.get('api')||'http://127.0.0.1:4174').replace(/\/$/,'')
const token=params.get('apiToken')||params.get('token')||''

async function request<T>(path:string,signal?:AbortSignal):Promise<T>{
  const separator=path.includes('?')?'&':'?'
  const response=await fetch(`${base}${path}${separator}token=${encodeURIComponent(token)}`,{signal})
  if(response.status===401)throw new Error('Analyzer rejected the access token')
  if(!response.ok)throw new Error(`Analyzer API ${response.status}: ${response.statusText}`)
  return response.json() as Promise<T>
}

export const api={
  workspace:(s?:AbortSignal)=>request<Workspace>('/api/workspace',s),
  events:(offset=0,limit=2000,s?:AbortSignal)=>request<Page<TraceEvent>>(`/api/events?offset=${offset}&limit=${limit}`,s),
  network:(offset=0,limit=2000,s?:AbortSignal)=>request<Page<NetworkEvidence>>(`/api/network?offset=${offset}&limit=${limit}`,s),
  packet:(id:string,s?:AbortSignal)=>request<NetworkEvidence>(`/api/packet?id=${encodeURIComponent(id)}`,s),
  chains:(offset=0,limit=500,s?:AbortSignal)=>request<Page<CausalityChain>>(`/api/chains?offset=${offset}&limit=${limit}`,s),
  diagnostics:(s?:AbortSignal)=>request<Diagnostic[]>('/api/diagnostics',s),
  metrics:(s?:AbortSignal)=>request<Metrics>('/api/metrics',s),
  exportUrl:`${base}/api/export?token=${encodeURIComponent(token)}`,
}
