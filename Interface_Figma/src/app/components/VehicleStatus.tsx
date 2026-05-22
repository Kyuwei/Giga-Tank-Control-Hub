import { memo, useEffect, useMemo, useState } from "react";
import {
  ArrowLeft, Users, Crosshair, Gauge, Activity, Shield, Flame, AlertCircle,
} from "lucide-react";
import { useNavigate } from "react-router";
import {
  fetchTelemetry,
  fetchModules,
  extractVehicleName,
  type WtTelemetry,
  type WtModuleHealth,
} from "../api";

function gearLabel(gear?: number | string): string {
  if (gear === undefined || gear === null) return "—";
  if (String(gear) === "0") return "N";
  if (String(gear).startsWith("-") || gear === "R") return "R";
  return String(gear);
}

interface ModuleRowProps {
  name: string;
  pct:  number;
}

const ModuleRow = memo(function ModuleRow({ name, pct }: ModuleRowProps) {
  const barColor = pct > 50 ? "bg-[#39ff14]" : pct > 25 ? "bg-yellow-400" : "bg-red-500 animate-pulse";
  const txtColor = pct > 50 ? "text-[#39ff14]" : pct > 25 ? "text-yellow-400" : "text-red-400";
  return (
    <div className="flex items-center gap-3 text-sm">
      <span className="w-32 text-[#39ff14]/70 tracking-wider">{name}</span>
      <div className="flex-1 h-3 bg-[#39ff14]/10 border border-[#39ff14]/20 overflow-hidden">
        <div className={`h-full transition-all ${barColor}`} style={{ width: `${pct}%` }} />
      </div>
      <span className={`w-12 text-right font-mono font-bold ${txtColor}`}>{pct}%</span>
    </div>
  );
});

export function VehicleStatus() {
  const navigate = useNavigate();
  const [data, setData]       = useState<WtTelemetry | null>(null);
  const [online, setOnline]   = useState(false);
  const [modules, setModules] = useState<WtModuleHealth | null>(null);

  useEffect(() => {
    let alive = true;
    const poll = async () => {
      const t = await fetchTelemetry();
      if (!alive) return;
      if (t && !t.error) { setData(t); setOnline(true); }
      else { setOnline(false); }
    };
    poll();
    const id = setInterval(poll, 500);
    return () => { alive = false; clearInterval(id); };
  }, []);

  useEffect(() => {
    let alive = true;
    const poll = async () => {
      const m = await fetchModules();
      if (alive && m) setModules(m);
    };
    poll();
    const id = setInterval(poll, 1000);
    return () => { alive = false; clearInterval(id); };
  }, []);

  const derived = useMemo(() => ({
    speed:       data?.speed  !== undefined ? Math.abs(Math.round(data.speed))  : "—",
    rpm:         data?.rpm    !== undefined ? Math.round(data.rpm)               : "—",
    gear:        gearLabel(data?.gear),
    fuel:        data?.fuel   !== undefined ? Math.round(data.fuel)              : null,
    ammo:        data?.first_stage_ammo,
    crew:        data?.crew_current !== undefined && data?.crew_total !== undefined
                  ? { current: data.crew_current, total: data.crew_total,
                      pct: data.crew_total > 0 ? Math.round((data.crew_current / data.crew_total) * 100) : 0 }
                  : null,
    vehicleName: extractVehicleName(data?.type) || "M1A2 SEP v3",
    driverDown:  (data?.driver_state ?? 0) !== 0,
    gunnerDown:  (data?.gunner_state ?? 0) !== 0,
  }), [data]);

  const moduleRows = useMemo(() => ([
    { name: "ENGINE",       pct: modules?.ENGINE       ?? 100 },
    { name: "TRANSMISSION", pct: modules?.TRANSMISSION ?? 100 },
    { name: "TURRET DRV",   pct: modules?.TURRET       ?? 100 },
    { name: "GUN BARREL",   pct: modules?.BARREL       ?? 100 },
    { name: "TRACK L",      pct: modules?.TRACK_L      ?? 100 },
    { name: "TRACK R",      pct: modules?.TRACK_R      ?? 100 },
  ]), [modules]);

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
              <span className="text-[#39ff14]/60 text-sm">DIAGNOSTICS</span>
              {online
                ? <span className="animate-pulse text-[#39ff14]">LIVE</span>
                : <span className="text-red-500/80">BRIDGE OFFLINE</span>}
            </div>
          </div>
          <div className="text-right">
            <div className="text-[#39ff14]/60 text-sm tracking-widest">VEHICLE TELEMETRY</div>
            <div className="text-xl tracking-wider">{derived.vehicleName}</div>
          </div>
        </div>

        {/* Content */}
        <div className="flex-1 grid grid-cols-1 md:grid-cols-2 gap-6 overflow-hidden z-10">
          {/* Left column */}
          <div className="flex flex-col gap-6">
            {/* CREW STATUS */}
            <div className="border-2 border-[#39ff14]/30 p-4 bg-[#0a1a0a]">
              <div className="flex items-center gap-2 mb-4 border-b border-[#39ff14]/30 pb-2">
                <Users className="text-[#39ff14]" size={20} />
                <h2 className="text-lg tracking-widest font-bold">CREW STATUS</h2>
              </div>
              {derived.crew ? (
                <div className="space-y-3">
                  <div className="flex justify-between items-end">
                    <span className="text-[#39ff14]/70 text-sm">PERSONNEL</span>
                    <span className={`text-2xl font-bold ${derived.crew.current < derived.crew.total ? "text-red-400" : "text-[#39ff14]"}`}>
                      {derived.crew.current} / {derived.crew.total}
                    </span>
                  </div>
                  <div className="w-full h-3 bg-[#39ff14]/20 border border-[#39ff14]/30">
                    <div
                      className={`h-full transition-all ${derived.crew.pct > 50 ? "bg-[#39ff14]" : derived.crew.pct > 25 ? "bg-yellow-400" : "bg-red-500"}`}
                      style={{ width: `${derived.crew.pct}%` }}
                    />
                  </div>
                  <div className="text-right text-xs text-[#39ff14]/50 font-mono">{derived.crew.pct}% OPERATIONAL</div>

                  <div className="grid grid-cols-2 gap-2 pt-2 border-t border-[#39ff14]/20">
                    <div className="flex justify-between border border-[#39ff14]/20 p-2 text-sm">
                      <span className="text-[#39ff14]/70">DRIVER</span>
                      <span className={derived.driverDown ? "text-red-400 font-bold" : "text-[#39ff14] font-bold"}>
                        {online ? (derived.driverDown ? "DOWN" : "OK") : "—"}
                      </span>
                    </div>
                    <div className="flex justify-between border border-[#39ff14]/20 p-2 text-sm">
                      <span className="text-[#39ff14]/70">GUNNER</span>
                      <span className={derived.gunnerDown ? "text-red-400 font-bold" : "text-[#39ff14] font-bold"}>
                        {online ? (derived.gunnerDown ? "DOWN" : "OK") : "—"}
                      </span>
                    </div>
                  </div>
                  <div className="flex justify-between items-center border-t border-[#39ff14]/20 pt-2">
                    <span className="text-[#39ff14]/70 text-sm">STABILIZER</span>
                    <span className={`font-bold text-sm ${data?.stabilizer ? "text-[#39ff14]" : "text-[#39ff14]/40"}`}>
                      {data?.stabilizer ? "ARMED" : "—"}
                    </span>
                  </div>
                </div>
              ) : (
                <div className="grid grid-cols-2 gap-4">
                  {["COMMANDER", "GUNNER", "LOADER", "DRIVER"].map((role) => (
                    <div key={role} className="flex justify-between items-center border border-[#39ff14]/20 p-2">
                      <span className="text-[#39ff14]/70 text-sm">{role}</span>
                      <span className="font-bold text-[#39ff14]/40">—</span>
                    </div>
                  ))}
                </div>
              )}
            </div>

            {/* AMMUNITION */}
            <div className="border-2 border-[#39ff14]/30 p-4 bg-[#0a1a0a] flex-1">
              <div className="flex items-center gap-2 mb-4 border-b border-[#39ff14]/30 pb-2">
                <Crosshair className="text-[#39ff14]" size={20} />
                <h2 className="text-lg tracking-widest font-bold">AMMUNITION</h2>
              </div>
              <div className="space-y-3">
                <div className="flex flex-col">
                  <div className="flex justify-between items-end mb-1">
                    <span className="text-sm">MAIN ROUND (1ST STAGE)</span>
                    <span className={`text-xl font-bold ${derived.ammo !== undefined && derived.ammo < 5 ? "text-red-400 animate-pulse" : ""}`}>
                      {derived.ammo ?? "—"}
                    </span>
                  </div>
                  {derived.ammo !== undefined && (
                    <div className="w-full h-2 bg-[#39ff14]/20">
                      <div
                        className={`h-full ${derived.ammo > 10 ? "bg-[#39ff14]" : derived.ammo > 5 ? "bg-yellow-400" : "bg-red-500"}`}
                        style={{ width: `${Math.min(100, (derived.ammo / 40) * 100)}%` }}
                      />
                    </div>
                  )}
                </div>
                <p className="text-[#39ff14]/30 text-xs tracking-wider italic pt-2">
                  Secondary ammo counters not exposed by WT API.
                </p>
              </div>
            </div>
          </div>

          {/* Right column */}
          <div className="flex flex-col gap-6">
            {/* DRIVETRAIN */}
            <div className="border-2 border-[#39ff14]/30 p-4 bg-[#0a1a0a]">
              <div className="flex items-center gap-2 mb-4 border-b border-[#39ff14]/30 pb-2">
                <Gauge className="text-[#39ff14]" size={20} />
                <h2 className="text-lg tracking-widest font-bold">DRIVETRAIN</h2>
              </div>
              <div className="grid grid-cols-2 gap-4">
                <div className="border border-[#39ff14]/20 p-4 flex flex-col items-center justify-center relative">
                  <span className="text-[#39ff14]/50 text-xs absolute top-2 left-2">SPEED</span>
                  <span className="text-4xl font-bold mt-2">{derived.speed}</span>
                  <span className="text-xs text-[#39ff14]/70">KM/H</span>
                </div>
                <div className="border border-[#39ff14]/20 p-4 flex flex-col items-center justify-center relative">
                  <span className="text-[#39ff14]/50 text-xs absolute top-2 left-2">RPM</span>
                  <span className="text-4xl font-bold mt-2">{derived.rpm}</span>
                  <span className="text-xs text-[#39ff14]/70">ENGINE</span>
                </div>
                <div className="border border-[#39ff14]/20 p-4 flex flex-col items-center justify-center relative">
                  <span className="text-[#39ff14]/50 text-xs absolute top-2 left-2">GEAR</span>
                  <span className="text-4xl font-bold mt-2">{derived.gear}</span>
                  <span className="text-xs text-[#39ff14]/70">CURRENT</span>
                </div>
                <div className="border border-[#39ff14]/20 p-4 flex flex-col items-center justify-center relative">
                  <span className="text-[#39ff14]/50 text-xs absolute top-2 left-2">FUEL</span>
                  <span className="text-4xl font-bold mt-2">{derived.fuel ?? "—"}</span>
                  <span className="text-xs text-[#39ff14]/70">{derived.fuel !== null ? "%" : "N/A"}</span>
                </div>
              </div>
            </div>

            {/* MODULE INTEGRITY */}
            <div className="border-2 border-[#39ff14]/30 p-4 bg-[#0a1a0a] flex-1">
              <div className="flex items-center gap-2 mb-4 border-b border-[#39ff14]/30 pb-2">
                <Shield className="text-[#39ff14]" size={20} />
                <h2 className="text-lg tracking-widest font-bold">MODULE INTEGRITY</h2>
              </div>
              <div className="space-y-2">
                {moduleRows.map(({ name, pct }) => (
                  <ModuleRow key={name} name={name} pct={pct} />
                ))}
              </div>
              <div className="pt-3 mt-3 border-t border-[#39ff14]/10 space-y-1">
                <div className="flex items-center gap-3 text-sm">
                  <Flame className="text-[#39ff14]/70" size={14} />
                  <span className="w-32 text-[#39ff14]/70">ENGINE FIRE</span>
                  <div className="flex-1">
                    {data?.engine_on_fire
                      ? <span className="text-red-500 animate-pulse font-bold tracking-widest">⚠ ON FIRE</span>
                      : <span className={online ? "text-[#39ff14]" : "text-[#39ff14]/30"}>NOMINAL</span>}
                  </div>
                </div>
                <div className="flex items-center gap-3 text-sm">
                  <AlertCircle className="text-[#39ff14]/70" size={14} />
                  <span className="w-32 text-[#39ff14]/70">STABILIZER</span>
                  <span className={data?.stabilizer ? "text-[#39ff14] font-bold" : "text-[#39ff14]/30"}>
                    {online ? (data?.stabilizer ? "ARMED" : "STANDBY") : "—"}
                  </span>
                </div>
              </div>
              <p className="text-[#39ff14]/25 text-xs tracking-wider italic pt-3 border-t border-[#39ff14]/10 mt-3">
                Module health synthesised from /hudmsg damage events. Resets at each new map.
              </p>
            </div>
          </div>
        </div>

        {/* Bottom Bar */}
        <div className="border-t-2 border-[#39ff14]/50 pt-2 mt-4 flex justify-between shrink-0 text-sm tracking-widest text-[#39ff14]/80">
          <div className="flex items-center gap-2">
            <Activity size={16} />
            {online ? "TELEMETRY: LIVE" : "TELEMETRY: BRIDGE OFFLINE"}
          </div>
          <div>BMS: {online ? "CONNECTED" : "—"}</div>
          <div>STAB: {data?.stabilizer ? "DUAL-AXIS ON" : "—"}</div>
        </div>
      </div>
    </div>
  );
}
