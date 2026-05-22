import { useEffect, useMemo, useState } from "react";
import { ArrowLeft } from "lucide-react";
import { useNavigate } from "react-router";
import {
  fetchMapData,
  classifyEntity,
  entityRotation,
  mapImageUrl,
  type WtMapObject,
  type WtMapInfo,
  type EntityKind,
} from "../api";
import { MapIcon } from "./MapIcon";

const KIND_COLOUR: Record<EntityKind, string> = {
  tank_ally:      "#39ff14",
  tank_enemy:     "#ef4444",
  aircraft_ally:  "#39ff14",
  aircraft_enemy: "#ef4444",
  objective:      "#facc15",
  airfield:       "#60a5fa",
  bomb_point:     "#ef4444",
  respawn:        "#396afa",
  unknown:        "#aaaaaa",
};

const LEGEND_KINDS: EntityKind[] = [
  "tank_ally", "tank_enemy", "aircraft_ally", "aircraft_enemy",
  "objective", "airfield", "bomb_point", "respawn",
];

const LEGEND_LABEL: Record<EntityKind, string> = {
  tank_ally:      "ALLY",
  tank_enemy:     "ENEMY",
  aircraft_ally:  "AIR-A",
  aircraft_enemy: "AIR-E",
  objective:      "OBJ",
  airfield:       "AIRFLD",
  bomb_point:     "BOMB",
  respawn:        "RSPWN",
  unknown:        "?",
};

export function TacticalMap() {
  const navigate = useNavigate();
  const [objects, setObjects] = useState<WtMapObject[]>([]);
  const [mapInfo, setMapInfo] = useState<WtMapInfo | null>(null);
  const [online, setOnline]   = useState(false);
  const [imgKey, setImgKey]   = useState(0);

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

  // Classify + rotate once per polling cycle, so MapIcon (memoised) can skip re-renders.
  const entities = useMemo(() => objects.map((obj, i) => {
    const kind = classifyEntity(obj);
    const rot  = entityRotation(obj);
    return {
      key:   i,
      kind,
      left:  obj.x * 100,
      top:   obj.y * 100,
      rot,
      label: obj.type ? `${obj.type.toUpperCase()} (${(obj.x * 100).toFixed(1)}, ${(obj.y * 100).toFixed(1)})` : undefined,
    };
  }), [objects]);

  return (
    <div className="flex items-center justify-center w-full h-screen bg-black select-none p-2 sm:p-4">
      <div
        className="relative w-full h-full max-w-6xl bg-[#051005] border-4 border-[#103010] rounded-xl overflow-hidden p-4 flex flex-col"
        style={{ boxShadow: "inset 0 0 100px rgba(0, 50, 0, 0.8), 0 0 50px rgba(57,255,20,0.1)" }}
      >
        <div className="absolute inset-0 pointer-events-none opacity-20 bg-[linear-gradient(rgba(18,16,16,0)_50%,rgba(0,0,0,0.25)_50%),linear-gradient(90deg,rgba(255,0,0,0.06),rgba(0,255,0,0.02),rgba(0,0,255,0.06))] bg-[length:100%_4px,3px_100%] z-50"></div>

        {/* Header */}
        <div className="flex justify-between items-end border-b-2 border-[#39ff14]/50 pb-2 mb-4 shrink-0">
          <div className="flex gap-4 items-center">
            <button
              onClick={() => navigate("/")}
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

        {/* Map area */}
        <div className="flex-1 relative border-2 border-[#39ff14]/30 overflow-hidden bg-[#0a1a0a] flex items-center justify-center">
          <img
            key={imgKey}
            src={mapImageUrl()}
            alt="Tactical map"
            className="absolute inset-0 w-full h-full object-cover opacity-30"
            onError={(e) => { (e.currentTarget as HTMLImageElement).style.display = "none"; }}
          />

          <div className="absolute inset-0 bg-[linear-gradient(rgba(57,255,20,0.07)_1px,transparent_1px),linear-gradient(90deg,rgba(57,255,20,0.07)_1px,transparent_1px)] bg-[size:40px_40px]"></div>

          <div className="absolute w-[800px] h-[800px] rounded-full border border-[#39ff14]/10 flex items-center justify-center pointer-events-none">
            <div className="w-[600px] h-[600px] rounded-full border border-[#39ff14]/10 flex items-center justify-center">
              <div className="w-[400px] h-[400px] rounded-full border border-[#39ff14]/10 flex items-center justify-center">
                <div className="w-[200px] h-[200px] rounded-full border border-[#39ff14]/10"></div>
              </div>
            </div>
            <div className="absolute top-1/2 left-1/2 w-[400px] h-[2px] bg-gradient-to-r from-transparent via-[#39ff14]/40 to-[#39ff14] origin-left animate-[spin_4s_linear_infinite]"></div>
          </div>

          <div className="relative w-full h-full z-10">
            {entities.map((e) => (
              <MapIcon
                key={e.key}
                kind={e.kind}
                left={e.left}
                top={e.top}
                rotation={e.rot}
                label={e.label}
              />
            ))}

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
          <div className="flex gap-3 flex-wrap">
            {LEGEND_KINDS.map((kind) => (
              <div key={kind} className="flex items-center gap-1">
                <div className="w-2 h-2 rounded-full" style={{ background: KIND_COLOUR[kind] }} />
                <span className="text-xs" style={{ color: KIND_COLOUR[kind] }}>{LEGEND_LABEL[kind]}</span>
              </div>
            ))}
          </div>
          <div>{online ? `ENTITIES: ${objects.length}` : "NO SIGNAL"}</div>
        </div>
      </div>
    </div>
  );
}
