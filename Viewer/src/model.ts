import type{CausalityChain,Diagnostic,TimelineEvent,TraceEvent}from'./types'

export const shortName=(path:string)=>path.split(/[.:/]/).filter(Boolean).at(-1)||path||'—'
export const eventStatus=(event:TraceEvent):TimelineEvent['status']=>event.type.includes('Failed')||/reject/i.test(event.detail)?'rejected':event.lane===2||event.lane===5?'network':event.role.includes('Server')?'authority':event.lane===1?'predicted':'accepted'

export function selectTimelineEvents(events:TraceEvent[],endpoint:string,query:string,minTime:number):TimelineEvent[]{
  const needle=query.toLowerCase()
  return events.filter(event=>(endpoint==='All endpoints'||event.endpoint===endpoint)&&`${event.type} ${event.subject} ${event.asc} ${event.detail}`.toLowerCase().includes(needle)).map(event=>({...event,timeMs:(event.time-minTime)*1000,label:shortName(event.subject)||event.type,status:eventStatus(event),kind:event.type,ability:shortName(event.subject),prediction:`${event.predictionCurrent}/${event.predictionBase} · conn ${event.predictiveConnectionKey}`,confidence:event.networkConfidence??(event.timingReliable?1:.5),packet:event.networkEvidenceId}))
}

export function diagnosticEventId(diagnostic:Diagnostic,events:TraceEvent[],chains:CausalityChain[]):string|undefined{
  if(events.some(event=>event.id===diagnostic.evidenceId))return diagnostic.evidenceId
  const chain=chains.find(item=>item.id===diagnostic.evidenceId||item.key===diagnostic.evidenceId)
  return chain?.stages.find(stage=>events.some(event=>event.id===stage.eventId))?.eventId
}
