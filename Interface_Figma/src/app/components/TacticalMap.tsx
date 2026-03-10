import React, { useState, useEffect } from "react";
import { ArrowLeft, Navigation } from "lucide-react";
import { useNavigate } from "react-router";
import {
  fetchMapData,
  classifyEntity,
  mapImageUrl,
  type WtMapObject,
  type WtMapInfo,
  type EntityKind,
} from "../api";

// Color palette per entity kind
const KIND_COLOR: Record<EntityKind, string> = {
  ally:      "#39ff14",
  enemy:     "#ef4444",
  objective: "#facc15",
  airfield:  "#60a5fa",
  unknown:   "#ffffff",
};

export function TacticalMap() {
  const navigate = useNavigate();
  const [objects, setObjects]  = useState<WtMapObject[]>([]);
  const [mapInfo, setMapInfo]  = useState<WtMapInfo | null>(null);
  const [online, setOnline]    = useState(false);
  // Bust the img cache on every new map generation
  const [imgKey, setImgKey]    = useState(0);

  useEffect(() => {
    let alive = true;
    let lastGen = -1;

    const poll = async () => {
      const data = await fetchMapData();
      if (!alive) return;
      if (data && !data.error) {
        setObjects(data.objects ?? []);
        setMapInfo(data.info);
        setOnline(true);
        const gen = data.info?.map_generation ?? 0;
        if (gen !== lastGen) { lastGen = gen; setImgKey((k) => k + 1); }
      } else {
        setOnline(false);
      }
    };

    poll();
    const id = setInterval(poll, 500);
    return () => { alive = false; clearInterval(id); };
  }, []);

  const mapName = mapInfo?.name?.toUpperCase() ?? "NO SIGNAL";

  return (
    <div className="flex items-center justify-center w-full h-screen bg-black select-none p-2 sm:p-4">
      {/* Container simulating the display */}
      <div 
        className="relative w-full h-full max-w-6xl bg-[#051005] border-4 border-[#103010] rounded-xl overflow-hidden p-4 shadow-[0_0_50px_rgba(57,255,20,0.1)] flex flex-col"
        style={{
          boxShadow: "inset 0 0 100px rgba(0, 50, 0, 0.8)",
        }}
      >
        {/* CRT Scanline overlay */}
        <div className="absolute inset-0 pointer-events-none opacity-20 bg-[linear-gradient(rgba(18,16,16,0)_50%,rgba(0,0,0,0.25)_50%),linear-gradient(90deg,rgba(255,0,0,0.06),rgba(0,255,0,0.02),rgba(0,0,255,0.06))] bg-[length:100%_4px,3px_100%] z-50"></div>
        
        {/* Top Header/Status Bar */}
        <div className="flex justify-between items-end border-b-2 border-[#39ff14]/50 pb-2 mb-4 shrink-0">
          <div className="flex gap-4 items-center">
            <button 
              onClick={() => navigate('/')}
              className="p-2 border-2 border-[#39ff14] text-[#39ff14] hover:bg-[#39ff14]/20 active:bg-[#39ff14]/40 transition-colors shadow-[0_0_10px_rgba(57,255,20,0.3)]"
            >
              <ArrowLeft size={24} />
            </button>
            <div className="flex flex-col">
              <span className="text-[#39ff14]/60 text-sm">SAT-LINK</span>
              {online
                ? <span className="animate-pulse text-[#39ff14]">CONNECTED</span>
                : <span className="text-red-500/80">BRIDGE OFFLINE</span>}
            </div>
          </div>
          
          <div className="text-right">
            <div className="text-[#39ff14]/60 text-sm tracking-widest">TACTICAL GRID // {mapName}</div>
          </div>
        </div>

        {/* Map Area */}
        <div className="flex-1 relative border-2 border-[#39ff14]/30 overflow-hidden bg-[#0a1a0a] flex items-center justify-center">
          {/* Map background image from WT */}
          <img
            key={imgKey}
            src={mapImageUrl()}
            alt="Tactical map"
            className="absolute inset-0 w-full h-full object-cover opacity-30"
            onError={(e) => { (e.currentTarget as HTMLImageElement).style.display = 'none'; }}
          />

          {/* Grid lines */}
          <div className="absolute inset-0 bg-[linear-gradient(rgba(57,255,20,0.07)_1px,transparent_1px),linear-gradient(90deg,rgba(57,255,20,0.07)_1px,transparent_1px)] bg-[size:40px_40px]"></div>

          {/* Radar rings */}
          <div className="absolute w-[800px] h-[800px] rounded-full border border-[#39ff14]/10 flex items-center justify-center pointer-events-none">
            <div className="w-[600px] h-[600px] rounded-full border border-[#39ff14]/10 flex items-center justify-center">
              <div className="w-[400px] h-[400px] rounded-full border border-[#39ff14]/10 flex items-center justify-center">
                <div className="w-[200px] h-[200px] rounded-full border border-[#39ff14]/10"></div>
              </div>
            </div>
            {/* Sweep */}
            <div className="absolute top-1/2 left-1/2 w-[400px] h-[2px] bg-gradient-to-r from-transparent via-[#39ff14]/40 to-[#39ff14] origin-left animate-[spin_4s_linear_infinite]"></div>
          </div>

          {/* Dynamic entity markers */}
          <div className="relative w-full h-full z-10">
            {objects.map((obj, i) => {
              const kind  = classifyEntity(obj.color ?? "", obj.type ?? "");
              const color = KIND_COLOR[kind];
              const left  = `${(obj.x * 100).toFixed(2)}%`;
              const top   = `${(obj.y * 100).toFixed(2)}%`;
              return (
                <div
                  key={i}
                  className="absolute"
                  style={{ left, top, transform: "translate(-50%, -50%)" }}
                >
                  <div
                    className={`w-3 h-3 rounded-full border-2 shadow-lg ${kind === "enemy" ? "animate-pulse" : ""}`}
                    style={{
                      backgroundColor: `${color}33`,
                      borderColor:      color,
                      boxShadow:        `0 0 6px ${color}`,
                    }}
                  />
                </div>
              );
            })}

            {/* No-signal overlay */}
            {!online && (
              <div className="absolute inset-0 flex items-center justify-center">
                <span className="text-[#39ff14]/30 text-xl tracking-widest uppercase">No Signal</span>
              </div>
            )}
          </div>
        </div>

        {/* Bottom Bar */}
        <div className="border-t-2 border-[#39ff14]/50 pt-2 mt-4 flex justify-between shrink-0 text-sm tracking-widest text-[#39ff14]/80">
          <div>ZOOM: 1X</div>
          <div className="flex gap-4">
            {Object.entries(KIND_COLOR).filter(([k]) => k !== "unknown").map(([kind, color]) => (
              <div key={kind} className="flex items-center gap-1">
                <div className="w-2 h-2 rounded-full" style={{ background: color }} />
                <span className="text-xs" style={{ color }}>{kind.toUpperCase()}</span>
              </div>
            ))}
          </div>
          <div>{online ? `ENTITIES: ${objects.length}` : "NO SIGNAL"}</div>
        </div>
      </div>
    </div>
  );
}
