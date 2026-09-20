import{describe,expect,it}from'vitest'
import{eventStatus,selectTimelineEvents,shortName}from'./model'
import type{TraceEvent}from'./types'

const event=(overrides:Partial<TraceEvent>={}):TraceEvent=>({id:'Client0:9007199254740993',endpoint:'Client0',role:'Client',type:'AbilityAttempt',typeId:0,lane:0,time:10.25,localTime:10.25,asc:'/Game/PS.ASC',subject:'/Game/GA_Fire.GA_Fire_C',detail:'Attempted',spec:17,predictionCurrent:84,predictionBase:81,predictiveConnectionKey:'9007199254740995',connectionId:1,valueA:0,valueB:0,flags:0,timingReliable:true,clockUncertaintySeconds:.001,...overrides})

describe('workspace event projection',()=>{
  it('preserves 64-bit identifiers as strings',()=>{const result=selectTimelineEvents([event()],'All endpoints','fire',10);expect(result[0].id).toBe('Client0:9007199254740993');expect(result[0].predictiveConnectionKey).toBe('9007199254740995')})
  it('applies endpoint and text filters and server-clock transform',()=>{const result=selectTimelineEvents([event()],'Client0','attempt',10);expect(result).toHaveLength(1);expect(result[0].timeMs).toBe(250)})
  it('encodes rejection by shape status, not color alone',()=>expect(eventStatus(event({detail:'ProjectValidationRejected'}))).toBe('rejected'))
  it('normalizes object labels',()=>expect(shortName('/Game/GA_Fire.GA_Fire_C')).toBe('GA_Fire_C'))
})
