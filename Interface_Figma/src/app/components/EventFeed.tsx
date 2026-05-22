import { memo, useEffect, useState } from "react";
import { Crosshair, Flame, AlertTriangle } from "lucide-react";
import { fetchEvents, type WtEvent } from "../api";

interface EventRowProps {
  ev: WtEvent;
  now: number;
}

const EventRow = memo(function EventRow({ ev, now }: EventRowProps) {
  const age = Math.max(0, now - ev.ts);
  const colour = ev.kind === "kill"
    ? "text-[#39ff14]"
    : ev.kind === "damage"
      ? "text-red-400"
      : "text-yellow-400";
  const Icon = ev.kind === "kill"
    ? Crosshair
    : ev.kind === "damage"
      ? Flame
      : AlertTriangle;
  return (
    <div className={`flex items-center gap-2 text-xs tracking-wider font-mono ${colour}`}>
      <Icon size={12} className="shrink-0" />
      <span className="text-[#39ff14]/40 w-12 shrink-0 text-right">{age}s</span>
      <span className="truncate">{ev.msg}</span>
    </div>
  );
});

interface Props {
  className?: string;
  limit?: number;
}

export const EventFeed = memo(function EventFeed({ className, limit = 10 }: Props) {
  const [events, setEvents] = useState<WtEvent[]>([]);
  const [now, setNow]       = useState(() => Math.floor(Date.now() / 1000));

  useEffect(() => {
    let alive = true;
    const poll = async () => {
      const list = await fetchEvents();
      if (alive) setEvents(list);
    };
    poll();
    const id = setInterval(poll, 1000);
    return () => { alive = false; clearInterval(id); };
  }, []);

  useEffect(() => {
    const id = setInterval(() => setNow(Math.floor(Date.now() / 1000)), 1000);
    return () => clearInterval(id);
  }, []);

  const visible = events.slice(-limit).reverse();

  return (
    <div className={`border-2 border-[#39ff14]/30 bg-[#0a1a0a] p-3 flex flex-col gap-1 ${className ?? ""}`}>
      <div className="flex items-center justify-between border-b border-[#39ff14]/30 pb-1 mb-1">
        <span className="text-[#39ff14] text-sm tracking-widest">EVENT FEED</span>
        <span className="text-[#39ff14]/40 text-xs">{events.length} TOTAL</span>
      </div>
      {visible.length === 0
        ? <span className="text-[#39ff14]/30 italic text-xs">No events yet — waiting for HUD messages…</span>
        : visible.map((ev, i) => (
            <EventRow key={`${ev.ts}-${i}`} ev={ev} now={now} />
          ))}
    </div>
  );
});
