import { useDeferredValue, useEffect, useMemo, useRef, useState } from 'react'
import { api } from './api'
import { diagnosticEventId, eventStatus, selectTimelineEvents, shortName } from './model'
import { Timeline } from './Timeline'
import type { CausalityChain, Diagnostic, Metrics, NetworkEvidence, TimelineEvent, TraceEvent, Workspace } from './types'
import './app.css'
import './contract.css'

const stages = ['Intent', 'Prediction', 'Network Out', 'Server Verdict', 'Authoritative State', 'Network In', 'Convergence']
const colors = ['#5bd6ff', '#ffb74d', '#9ee37d', '#b7a9ff']
type DetailTab = 'diagnostics' | 'metrics' | 'bookmarks'

function readBookmarks(captureId: string): string[] {
  try {
    const value = JSON.parse(localStorage.getItem(`gnt-bookmarks:${captureId}`) || '[]')
    return Array.isArray(value) ? value.filter((item): item is string => typeof item === 'string') : []
  } catch { return [] }
}
function metricText(value: Metrics[string]): string {
  return typeof value === 'number'
    ? value.toLocaleString()
    : `${value.count.toLocaleString()} samples · p50 ${value.p50Ms.toFixed(1)} ms · p95 ${value.p95Ms.toFixed(1)} ms`
}

export default function App() {
  const [workspace, setWorkspace] = useState<Workspace | null>(null)
  const [events, setEvents] = useState<TraceEvent[]>([])
  const [network, setNetwork] = useState<NetworkEvidence[]>([])
  const [chains, setChains] = useState<CausalityChain[]>([])
  const [diagnostics, setDiagnostics] = useState<Diagnostic[]>([])
  const [metrics, setMetrics] = useState<Metrics>({})
  const [selected, setSelected] = useState(() => new URLSearchParams(location.search).get('event') || '')
  const [query, setQuery] = useState('')
  const [endpoint, setEndpoint] = useState('All endpoints')
  const [bookmarks, setBookmarks] = useState<string[]>([])
  const [activeTab, setActiveTab] = useState<DetailTab>('diagnostics')
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState('')
  const [packetDetail, setPacketDetail] = useState<NetworkEvidence | null>(null)
  const eventListRef = useRef<HTMLDivElement>(null)
  const deferredQuery = useDeferredValue(query)

  const load = () => {
    const controller = new AbortController()
    setLoading(true); setError('')
    Promise.all([
      api.workspace(controller.signal), api.events(0, 2000, controller.signal),
      api.network(0, 2000, controller.signal), api.chains(0, 500, controller.signal),
      api.diagnostics(controller.signal), api.metrics(controller.signal),
    ]).then(([w, e, n, c, d, m]) => {
      setBookmarks(readBookmarks(w.captureId))
      setWorkspace(w); setEvents(e.items); setNetwork(n.items); setChains(c.items)
      setDiagnostics(d); setMetrics(m)
      setSelected(current => e.items.some(item => item.id === current) ? current : e.items[0]?.id || '')
      setLoading(false)
    }).catch(e => { if (e.name !== 'AbortError') { setError(e.message || String(e)); setLoading(false) } })
    return () => controller.abort()
  }
  useEffect(load, [])
  useEffect(() => {
    if (!selected) return
    const url = new URL(location.href)
    url.searchParams.set('event', selected)
    history.replaceState(null, '', url)
  }, [selected])
  useEffect(() => {
    if (workspace) localStorage.setItem(`gnt-bookmarks:${workspace.captureId}`, JSON.stringify(bookmarks))
  }, [bookmarks, workspace])

  const minTime = useMemo(() => events.length ? Math.min(...events.map(item => item.time)) : 0, [events])
  const visible = useMemo<TimelineEvent[]>(
    () => selectTimelineEvents(events, endpoint, deferredQuery, minTime),
    [events, endpoint, deferredQuery, minTime],
  )
  const event = events.find(item => item.id === selected)
  const selectedVisible = visible.some(item => item.id === selected)
  const cachedPacket = network.find(item => item.id === event?.networkEvidenceId)
  useEffect(() => {
    setPacketDetail(null)
    if (!event?.networkEvidenceId || cachedPacket) return
    const controller = new AbortController()
    api.packet(event.networkEvidenceId, controller.signal).then(setPacketDetail).catch(() => {})
    return () => controller.abort()
  }, [event?.networkEvidenceId, cachedPacket])
  const packet = cachedPacket ?? (packetDetail?.id === event?.networkEvidenceId ? packetDetail : undefined)
  const chain = chains.find(item => item.stages.some(stage => stage.eventId === selected))
  const selectedIndex = visible.findIndex(item => item.id === selected)
  useEffect(() => {
    const list = eventListRef.current
    const row = list?.querySelector<HTMLElement>('.event-row.selected')
    if (!list || !row) return
    const offset = row.getBoundingClientRect().top - list.getBoundingClientRect().top
    if (offset < 36) list.scrollTop += offset - 36
    else if (offset + row.offsetHeight > list.clientHeight) list.scrollTop += offset + row.offsetHeight - list.clientHeight
  }, [selected, visible])
  const showEvent = (id: string) => { setQuery(''); setEndpoint('All endpoints'); setSelected(id) }
  const changeEndpoint = (value: string) => {
    setEndpoint(value)
    const first = events.find(item => (value === 'All endpoints' || item.endpoint === value)
      && `${item.type} ${item.subject} ${item.asc} ${item.detail}`.toLowerCase().includes(query.toLowerCase()))
    if (first) setSelected(first.id)
  }
  const toggleBookmark = () => {
    if (selected) setBookmarks(items => items.includes(selected) ? items.filter(id => id !== selected) : [...items, selected])
  }

  if (loading) return <main className="state"><div className="spinner" /><h1>ANALYZING TRACE WORKSPACE</h1><p>Loading GAS events and network evidence…</p></main>
  if (error) return <main className="state error-state"><h1>WORKSPACE UNAVAILABLE</h1><p>{error}</p><button onClick={load}>RECONNECT</button></main>
  if (!workspace || events.length === 0) return <main className="state"><h1>NO GAS EVENTS</h1><p>No readable GASNetTrace events were returned.</p><button onClick={load}>REFRESH</button></main>

  return <main className="app">
    <header className="topbar">
      <div className="brand"><span className="mark">G·N·T</span><div><strong>GAS NET TRACE</strong><small>CAUSALITY WORKBENCH</small></div></div>
      <div className="capture"><span>CAPTURE</span><code title={workspace.captureId}>{workspace.captureId}</code><i>ANALYZER API</i></div>
      <div className="actions"><button onClick={load}>RELOAD</button><button onClick={toggleBookmark} aria-pressed={bookmarks.includes(selected)}>{bookmarks.includes(selected) ? 'SAVED' : 'SAVE EVENT'}</button><a className="primary" href={api.exportUrl} download="gas-net-trace-report.json">EXPORT REPORT</a></div>
    </header>
    <section className="toolbar" aria-label="Event filters">
      <div className="health"><b /> {workspace.endpoints.length} ENDPOINT{workspace.endpoints.length === 1 ? '' : 'S'} <span>{workspace.endpoints.every(item => item.clockSynchronized || item.role.includes('Server')) ? 'clocks aligned' : 'timing confidence limited'}</span></div>
      <input aria-label="Search events" value={query} onChange={e => setQuery(e.target.value)} placeholder="Search event, ability, ASC, prediction key…" />
      <select aria-label="Endpoint" value={endpoint} onChange={e => changeEndpoint(e.target.value)}><option>All endpoints</option>{workspace.endpoints.map(item => <option key={item.id}>{item.id}</option>)}</select>
      <span className="result-count">{visible.length} / {events.length} events</span>
    </section>
    <div className="workspace">
      <aside className="navigator panel">
        <div className="side-section"><h2>ENDPOINTS</h2><button className={endpoint === 'All endpoints' ? 'endpoint active' : 'endpoint'} onClick={() => changeEndpoint('All endpoints')}><span className="all-dot" /><b>All endpoints</b><small>{events.length} events</small></button>
          {workspace.endpoints.map((item, index) => <button className={endpoint === item.id ? 'endpoint active' : 'endpoint'} key={item.id} onClick={() => changeEndpoint(item.id)}><span style={{ background: colors[index % colors.length] }} /><b>{item.id}</b><small>{item.role.toUpperCase()} · {item.clockSynchronized ? `±${(item.clockUncertaintySeconds * 1000).toFixed(2)} ms` : 'ORDER ONLY'}</small></button>)}
        </div>
        <div className="side-section"><h2>COVERAGE MANIFEST</h2><div className="tree">{workspace.coverage.slice(0, 10).map(item => <span key={`${item.endpoint}:${item.ascId}`}><b>{item.endpoint}</b> ASC {item.ascId} <em>0x{item.mask.toString(16)}</em></span>)}</div></div>
        {workspace.analysisWarnings.length > 0 && <div className="coverage"><h2>ANALYSIS WARNINGS</h2>{workspace.analysisWarnings.map(item => <p key={item}>{item}</p>)}</div>}
        <div className="side-section"><h2>WORKSPACE</h2><div className="tree"><span>Schema <em>{workspace.schema}</em></span><span>Events <em>{events.length}</em></span><span>Packets <em>{network.length}</em></span><span>Chains <em>{chains.length}</em></span></div></div>
      </aside>
      <section className="center">
        <div className="lens panel">
          <div className="section-title"><h2>CAUSALITY LENS</h2><span>{chain ? `${chain.edges.length} evidence edges · ${chain.complete ? 'complete' : 'incomplete'}` : 'No chain linked to this event'}</span></div>
          {chain ? <div className="chain">{stages.map((name, index) => {
            const stage = chain.stages.find(item => events.find(candidate => candidate.id === item.eventId)?.lane === index)
            return <button key={name} className={stage?.eventId === selected ? 'hot' : ''} onClick={() => stage && showEvent(stage.eventId)} disabled={!stage} title={stage ? `Open ${stage.type}` : 'No evidence for this stage'}><b>{String(index + 1).padStart(2, '0')}</b><span>{name}</span><small>{stage ? `${((stage.time - chain.stages[0].time) * 1000).toFixed(1)} ms` : 'NO EVIDENCE'}</small></button>
          })}</div> : <div className="no-chain">Select a chain event or use a diagnostic jump to inspect causal stages.</div>}
        </div>
        <div className="timeline panel">
          <div className="section-title"><h2>EVENT TIMELINE</h2><span>{visible.some(item => !item.timingReliable) ? 'Uncertain timing · event order only' : 'Server clock basis'}</span></div>
          {!selectedVisible && event && <div className="selection-note">Selected event is hidden by the current filters. <button onClick={() => showEvent(selected)}>SHOW EVENT</button></div>}
          <div className="timeline-body"><Timeline events={visible} selected={selected} onSelect={setSelected} />
            <div className="event-list" aria-label="Events" ref={eventListRef}>
              <div className="event-list-title"><b>EVENTS</b><span>{visible.length} shown</span></div>
              {visible.length ? visible.map(item => <button key={item.id} className={item.id === selected ? 'event-row selected' : 'event-row'} aria-current={item.id === selected ? 'true' : undefined} onClick={() => setSelected(item.id)}><time>+{item.timeMs.toFixed(0)} ms</time><span><b>{item.type}</b><small title={item.subject}>{shortName(item.subject)}</small></span><em>{item.endpoint}</em></button>) : <p className="empty-list">No events match these filters.</p>}
            </div>
          </div>
        </div>
        <div className="bottom panel">
          <div className="tabs" role="tablist" aria-label="Workspace details">
            <button role="tab" aria-selected={activeTab === 'diagnostics'} onClick={() => setActiveTab('diagnostics')}>DIAGNOSTICS <i>{diagnostics.length}</i></button>
            <button role="tab" aria-selected={activeTab === 'metrics'} onClick={() => setActiveTab('metrics')}>METRICS <i>{Object.keys(metrics).length}</i></button>
            <button role="tab" aria-selected={activeTab === 'bookmarks'} onClick={() => setActiveTab('bookmarks')}>BOOKMARKS <i>{bookmarks.length}</i></button>
          </div>
          <div className="detail-list" role="tabpanel">
            {activeTab === 'diagnostics' && (diagnostics.length ? diagnostics.map((item, index) => {
              const target = diagnosticEventId(item, events, chains)
              return <div className="diagnostic" key={`${item.id}-${index}`}><i className={item.severity} /><code>{item.id}</code><div><b>{item.summary}</b><small>{item.definitive ? 'deterministic' : 'evidence limited'} · {Math.round(item.confidence * 100)}% confidence</small></div>{target ? <button onClick={() => showEvent(target)} aria-label={`Open evidence for ${item.id}`}>OPEN EVIDENCE →</button> : <span className="no-target" title="This workspace-level diagnostic has no linked event">NO EVENT LINK</span>}</div>
            }) : <p className="empty-list">No diagnostics were reported.</p>)}
            {activeTab === 'metrics' && <div className="metric-list">{Object.entries(metrics).map(([key, value]) => <div key={key}><b>{key}</b><span>{metricText(value)}</span></div>)}</div>}
            {activeTab === 'bookmarks' && (bookmarks.length ? bookmarks.map(id => <button className="bookmark-row" key={id} onClick={() => showEvent(id)}>{id}<span>OPEN →</span></button>) : <p className="empty-list">Save an event to find it here later.</p>)}
          </div>
        </div>
      </section>
      <aside className="inspector panel">
        <div className="section-title"><h2>EVENT EVIDENCE</h2><span>{event?.id || 'None selected'}</span></div>
        {event && <><div className={`verdict ${eventStatus(event)}`}><small>{event.type}</small><b>{shortName(event.subject)}</b><span>{event.endpoint} · +{((event.time - minTime) * 1000).toFixed(2)} ms</span></div>
          <div className="event-controls"><button disabled={selectedIndex <= 0} onClick={() => setSelected(visible[selectedIndex - 1].id)}>← PREVIOUS</button><button disabled={selectedIndex < 0 || selectedIndex >= visible.length - 1} onClick={() => setSelected(visible[selectedIndex + 1].id)}>NEXT →</button></div>
          <dl><dt>ASC</dt><dd title={event.asc}>{event.asc || '—'}</dd><dt>SPEC HANDLE</dt><dd>{event.spec}</dd><dt>PREDICTION</dt><dd>{event.predictionCurrent}/{event.predictionBase} · {event.predictiveConnectionKey}</dd><dt>CONFIDENCE</dt><dd>{Math.round((event.networkConfidence ?? (event.timingReliable ? 1 : .5)) * 100)}%</dd><dt>CLOCK ERROR</dt><dd>{event.timingReliable ? `±${(event.clockUncertaintySeconds * 1000).toFixed(2)} ms` : 'ordering only'}</dd></dl>
          <h2>RAW DETAIL</h2><pre>{event.detail || 'No payload text (privacy-safe structured event)'}</pre>
          <h2>NETWORK EVIDENCE</h2>{packet ? <details className="packet"><summary>◆ packet {packet.sequence} · {packet.direction}<small>{packet.bytes} bytes · {packet.endpoint}</small></summary><div>{packet.content.join(' / ') || 'No packet content labels'}</div></details> : <div className="missing">No direct packet edge<br /><small>{event.networkMatchBasis || 'semantic evidence only'}</small></div>}
          <h2>MATCH BASIS</h2><ul><li>Capture ID exact</li><li>{event.networkMatchBasis || 'No network candidate'}</li><li>Confidence is evidence-derived</li><li>{event.timingReliable ? 'Clock ordering reliable' : 'Cross-endpoint milliseconds suppressed'}</li></ul>
        </>}
      </aside>
    </div>
  </main>
}
