import { useEffect, useRef } from 'react'
import type { TimelineEvent } from './types'

const lanes = [
  'CLIENT INTENT',
  'PREDICTION',
  'NETWORK OUT',
  'SERVER VERDICT',
  'AUTHORITATIVE STATE',
  'NETWORK IN',
  'CLIENT CONVERGENCE',
]

const colors: Record<TimelineEvent['status'], string> = {
  predicted: '#5bd6ff',
  accepted: '#9ee37d',
  rejected: '#ff6b6b',
  authority: '#ffb74d',
  network: '#b7a9ff',
}

function viewWindow(events: TimelineEvent[], selected: string) {
  const focus = events.find(item => item.id === selected)?.timeMs ?? events[0]?.timeMs ?? 0
  const start = Math.max(0, focus - 5000)
  return { start, end: start + 10000 }
}

export function Timeline({ events, selected, onSelect }: {
  events: TimelineEvent[]
  selected: string
  onSelect: (id: string) => void
}) {
  const ref = useRef<HTMLCanvasElement>(null)

  useEffect(() => {
    const canvas = ref.current
    if (!canvas) return
    const draw = () => {
    const dpr = devicePixelRatio || 1
    const width = canvas.clientWidth
    const height = canvas.clientHeight
    canvas.width = width * dpr
    canvas.height = height * dpr
    const context = canvas.getContext('2d')
    if (!context) return

    const left = 156
    const top = 34
    const laneHeight = (height - top) / lanes.length
    const { start, end } = viewWindow(events, selected)
    const plotWidth = Math.max(1, width - left - 24)
    context.scale(dpr, dpr)
    context.fillStyle = '#0c1015'
    context.fillRect(0, 0, width, height)
    context.font = '10px Consolas'
    context.textBaseline = 'middle'

    lanes.forEach((name, index) => {
      const y = top + index * laneHeight
      context.fillStyle = index % 2 ? '#0f141a' : '#0b1015'
      context.fillRect(left, y, width - left, laneHeight)
      context.strokeStyle = '#222a33'
      context.beginPath()
      context.moveTo(0, y)
      context.lineTo(width, y)
      context.stroke()
      context.fillStyle = '#73808d'
      context.fillText(name, 14, y + laneHeight / 2)
    })

    const gridStep = Math.max(100, Math.ceil(10000 / Math.max(1, Math.floor(plotWidth / 85)) / 100) * 100)
    for (let ms = Math.ceil(start / gridStep) * gridStep; ms <= end; ms += gridStep) {
      const x = left + ((ms - start) / (end - start)) * plotWidth
      context.strokeStyle = '#202833'
      context.beginPath()
      context.moveTo(x, top)
      context.lineTo(x, height)
      context.stroke()
      context.fillStyle = '#65717e'
      context.fillText(`+${(ms / 1000).toFixed(1)}s`, x + 4, 16)
    }

    // The Causality Lens owns causal edges. A global polyline through unrelated
    // visible events would imply evidence the analyzer never established.
    events.filter(event => event.timeMs >= start && event.timeMs <= end).forEach(event => {
      const x = left + ((event.timeMs - start) / (end - start)) * plotWidth
      const y = top + event.lane * laneHeight + laneHeight / 2
      const active = event.id === selected
      context.fillStyle = colors[event.status]
      context.strokeStyle = active ? '#fff' : colors[event.status]
      context.lineWidth = active ? 2 : 1
      if (event.status === 'network') {
        context.beginPath()
        context.moveTo(x, y - 6)
        context.lineTo(x + 6, y)
        context.lineTo(x, y + 6)
        context.lineTo(x - 6, y)
        context.closePath()
        context.fill()
      } else if (event.status === 'rejected') {
        context.beginPath()
        context.moveTo(x - 6, y - 6)
        context.lineTo(x + 6, y + 6)
        context.moveTo(x + 6, y - 6)
        context.lineTo(x - 6, y + 6)
        context.stroke()
      } else {
        context.beginPath()
        context.arc(x, y, active ? 6 : 4, 0, Math.PI * 2)
        context.fill()
        if (active) context.stroke()
      }
      if (active || events.length < 12) context.fillText(event.label, x + 10, y)
    })
    }
    draw()
    const observer = new ResizeObserver(draw)
    observer.observe(canvas)
    return () => observer.disconnect()
  }, [events, selected])

  return <canvas
    ref={ref}
    role="application"
    aria-label="Synchronized trace timeline"
    tabIndex={0}
    onKeyDown={event => {
      const index = events.findIndex(item => item.id === selected)
      if (event.key === 'ArrowRight' && index < events.length - 1) onSelect(events[index + 1].id)
      if (event.key === 'ArrowLeft' && index > 0) onSelect(events[index - 1].id)
    }}
    onClick={event => {
      const bounds = event.currentTarget.getBoundingClientRect()
      const pointerX = event.clientX - bounds.left
      const pointerY = event.clientY - bounds.top
      const left = 156
      const top = 34
      const laneHeight = (bounds.height - top) / lanes.length
      const { start, end } = viewWindow(events, selected)
      const hit = events.filter(item => item.timeMs >= start && item.timeMs <= end).map(item => ({
        event: item,
        x: left + ((item.timeMs - start) / (end - start)) * (bounds.width - left - 24),
        y: top + item.lane * laneHeight + laneHeight / 2,
      })).sort((a, b) =>
        (a.x - pointerX) ** 2 + (a.y - pointerY) ** 2 -
        ((b.x - pointerX) ** 2 + (b.y - pointerY) ** 2),
      )[0]
      if (hit && Math.hypot(hit.x - pointerX, hit.y - pointerY) < 28) onSelect(hit.event.id)
    }}
  />
}
