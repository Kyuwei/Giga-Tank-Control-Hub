import { useState, useEffect, useMemo, memo } from "react";
import { useNavigate } from "react-router";
import {
  Activity, Wifi, WifiOff, Gauge, Fuel, Users, ShieldAlert, Map as MapIcon,
  Flame, AlertTriangle, Radio, Eye, Crosshair, Target as TargetIcon,
} from "lucide-react";
import {
  checkBridgeHealth,
  fetchTelemetry,
  extractVehicleName,
  type WtTelemetry,
} from "../api";
import { EventFeed } from "./EventFeed";

interface WarningProps {
  label:   string;
  active:  boolean;
  icon:    typeof Flame;
}

const Warning = memo(function Warning({ label, active, icon: Icon }: WarningProps) {
  return (
    <div
      className={`flex items-center gap-2 px-2 py-1 border-2 transition-colors ${
        active
          ? "border-red-500 text-red-400 bg-red-500/10 animate-pulse"
          : "border-[#39ff14]/15 text-[#39ff14]/30 bg-[#0a1a0a]"
      }`}
      style={{
        boxShadow: active ? "0 0 12px rgba(239, 68, 68, 0.35)" : "none",
      }}
    >
      <Icon size={14} className="shrink-0" />
      <span className="text-xs tracking-widest truncate">{label}</span>
    </div>
  );
});

export function Dashboard() {
  const navigate = useNavigate();
  const [time, setTime]                 = useState(() => new Date().toLocaleTimeString("en-US", { hour12: false }));
  const [bridgeOnline, setBridgeOnline] = useState<boolean | null>(null);
  const [telemetry, setTelemetry]       = useState<WtTelemetry | null>(null);

  useEffect(() => {
    const id = setInterval(() => {
      setTime(new Date().toLocaleTimeString("en-US", { hour12: false }));
    }, 1000);
    return () => clearInterval(id);
  }, []);

  useEffect(() => {
    let alive = true;
    const check = async () => {
      const ok = await checkBridgeHealth();
      if (alive) setBridgeOnline(ok);
    };
    check();
    const id = setInterval(check, 3000);
    return () => { alive = false; clearInterval(id); };
  }, []);

  useEffect(() => {
    let alive = true;
    const poll = async () => {
      const t = await fetchTelemetry();
      if (alive) setTelemetry(t && !t.error ? t : null);
    };
    poll();
    const id = setInterval(poll, 500);
    return () => { alive = false; clearInterval(id); };
  }, []);

  // ─── Derived values, memoised so children with React.memo can skip re-renders ──
  const derived = useMemo(() => {
    const wtOnline    = telemetry !== null;
    const ammo        = telemetry?.first_stage_ammo;
    const crewCurrent = telemetry?.crew_current;
    const crewTotal   = telemetry?.crew_total;
    const vehicleName = extractVehicleName(telemetry?.type);
    const stabOn      = Boolean(telemetry?.stabilizer);
    const speed       = telemetry?.speed !== undefined ? Math.abs(Math.round(telemetry.speed)) : null;
    const rpm         = telemetry?.rpm   !== undefined ? Math.round(telemetry.rpm)              : null;
    const fuel        = telemetry?.fuel  !== undefined ? Math.round(telemetry.fuel)             : null;
    const gear        = telemetry?.gear !== undefined && telemetry?.gear !== null ? String(telemetry.gear) : "—";
    const driverDown  = (telemetry?.driver_state ?? 0) !== 0;
    const gunnerDown  = (telemetry?.gunner_state ?? 0) !== 0;
    const fire        = Boolean(telemetry?.engine_on_fire);
    const overspeed   = (telemetry?.has_speed_warning ?? 0) !== 0;
    const lws         = (telemetry?.lws ?? -1) > 0;
    const ircm        = (telemetry?.ircm ?? -1) > 0;
    const ammoLow     = ammo !== undefined && ammo < 5;
    const fuelLow     = fuel !== null && fuel < 20;
    return {
      wtOnline, ammo, crewCurrent, crewTotal, vehicleName, stabOn,
      speed, rpm, fuel, gear, driverDown, gunnerDown, fire, overspeed,
      lws, ircm, ammoLow, fuelLow,
    };
  }, [telemetry]);

  return (
    <div className="flex items-center justify-center w-full h-screen bg-black select-none p-2 sm:p-4">
      <div
        className="relative w-full h-full max-w-6xl bg-[#051005] border-4 border-[#103010] rounded-xl overflow-hidden p-4 flex flex-col"
        style={{ boxShadow: "inset 0 0 100px rgba(0, 50, 0, 0.8), 0 0 50px rgba(57,255,20,0.1)" }}
      >
        <div className="absolute inset-0 pointer-events-none opacity-20 bg-[linear-gradient(rgba(18,16,16,0)_50%,rgba(0,0,0,0.25)_50%),linear-gradient(90deg,rgba(255,0,0,0.06),rgba(0,255,0,0.02),rgba(0,0,255,0.06))] bg-[length:100%_4px,3px_100%] z-50"></div>

        {/* Header */}
        <div className="flex justify-between items-end border-b-2 border-[#39ff14]/50 pb-2 mb-3 shrink-0">
          <div className="flex gap-6 text-xl">
            <div className="flex flex-col">
              <span className="text-[#39ff14]/60 text-sm">SYSTEM</span>
              <span className="animate-pulse text-[#39ff14]">ONLINE</span>
            </div>
            <div className="flex flex-col">
              <span className="text-[#39ff14]/60 text-sm">BRIDGE</span>
              {bridgeOnline === null ? (
                <span className="text-[#39ff14]/50">INIT…</span>
              ) : bridgeOnline ? (
                <span className="flex items-center gap-1 text-[#39ff14]">
                  <Wifi size={14} /> ACTIVE
                </span>
              ) : (
                <span className="flex items-center gap-1 text-red-500 animate-pulse">
                  <WifiOff size={14} /> OFFLINE
                </span>
              )}
            </div>
            <div className="flex flex-col">
              <span className="text-[#39ff14]/60 text-sm">WAR THUNDER</span>
              <span className={derived.wtOnline ? "text-[#39ff14]" : "text-[#39ff14]/30"}>
                {derived.wtOnline ? "IN MATCH" : "STANDBY"}
              </span>
            </div>
          </div>

          <div className="flex-1 flex justify-center gap-4">
            <button
              className="px-6 py-2 border-2 border-[#39ff14] text-[#39ff14] hover:bg-[#39ff14]/20 active:bg-[#39ff14]/40 transition-colors shadow-[0_0_15px_rgba(57,255,20,0.3)] hover:shadow-[0_0_20px_rgba(57,255,20,0.5)] flex items-center gap-2 font-bold tracking-widest"
              onClick={() => navigate("/map")}
            >
              <MapIcon size={20} />
              TACTICAL MAP
            </button>
            <button
              className="px-6 py-2 border-2 border-[#39ff14] text-[#39ff14] hover:bg-[#39ff14]/20 active:bg-[#39ff14]/40 transition-colors shadow-[0_0_15px_rgba(57,255,20,0.3)] hover:shadow-[0_0_20px_rgba(57,255,20,0.5)] flex items-center gap-2 font-bold tracking-widest"
              onClick={() => navigate("/status")}
            >
              <Activity size={20} />
              SYS STATUS
            </button>
          </div>

          <div className="text-right">
            <div className="text-[#39ff14]/60 text-sm tracking-widest">
              {derived.wtOnline ? derived.vehicleName : "MFD"} // TACTICAL HUB
            </div>
            <div className="text-2xl tracking-wider">{time}</div>
          </div>
        </div>

        {/* Row 1 — VITALS | DRIVETRAIN | WEAPONS */}
        <div className="grid grid-cols-3 gap-3 mb-3">
          {/* VITALS */}
          <div className="border-2 border-[#39ff14]/30 bg-[#0a1a0a] p-3">
            <div className="flex items-center gap-2 border-b border-[#39ff14]/30 pb-1 mb-2">
              <Users size={16} className="text-[#39ff14]" />
              <h2 className="text-sm tracking-widest font-bold">VITALS</h2>
            </div>
            <div className="grid grid-cols-2 gap-x-3 gap-y-2 text-sm">
              <div className="flex justify-between">
                <span className="text-[#39ff14]/60">DRIVER</span>
                <span className={derived.driverDown ? "text-red-400 font-bold" : "text-[#39ff14] font-bold"}>
                  {derived.wtOnline ? (derived.driverDown ? "DOWN" : "OK") : "—"}
                </span>
              </div>
              <div className="flex justify-between">
                <span className="text-[#39ff14]/60">GUNNER</span>
                <span className={derived.gunnerDown ? "text-red-400 font-bold" : "text-[#39ff14] font-bold"}>
                  {derived.wtOnline ? (derived.gunnerDown ? "DOWN" : "OK") : "—"}
                </span>
              </div>
              <div className="col-span-2 flex justify-between items-center">
                <span className="text-[#39ff14]/60">CREW</span>
                <span className={
                  derived.crewCurrent !== undefined && derived.crewTotal !== undefined && derived.crewCurrent < derived.crewTotal
                    ? "text-red-400 font-bold" : "text-[#39ff14] font-bold"
                }>
                  {derived.crewCurrent !== undefined && derived.crewTotal !== undefined
                    ? `${derived.crewCurrent}/${derived.crewTotal}`
                    : "—"}
                </span>
              </div>
              <div className="col-span-2">
                <div className="w-full h-1.5 bg-[#39ff14]/10 border border-[#39ff14]/20">
                  {derived.crewCurrent !== undefined && derived.crewTotal !== undefined && derived.crewTotal > 0 && (
                    <div
                      className={`h-full transition-all ${
                        derived.crewCurrent / derived.crewTotal > 0.5 ? "bg-[#39ff14]"
                          : derived.crewCurrent / derived.crewTotal > 0.25 ? "bg-yellow-400" : "bg-red-500"
                      }`}
                      style={{ width: `${(derived.crewCurrent / derived.crewTotal) * 100}%` }}
                    />
                  )}
                </div>
              </div>
              <div className="col-span-2 flex justify-between items-center pt-1 border-t border-[#39ff14]/20">
                <span className="text-[#39ff14]/60 flex items-center gap-1">
                  <ShieldAlert size={12} /> STABILIZER
                </span>
                <span className={derived.stabOn ? "text-[#39ff14] font-bold" : "text-[#39ff14]/30"}>
                  {derived.wtOnline ? (derived.stabOn ? "ARMED" : "OFF") : "—"}
                </span>
              </div>
            </div>
          </div>

          {/* DRIVETRAIN */}
          <div className="border-2 border-[#39ff14]/30 bg-[#0a1a0a] p-3">
            <div className="flex items-center gap-2 border-b border-[#39ff14]/30 pb-1 mb-2">
              <Gauge size={16} className="text-[#39ff14]" />
              <h2 className="text-sm tracking-widest font-bold">DRIVETRAIN</h2>
            </div>
            <div className="grid grid-cols-3 gap-2 text-center mb-2">
              <div>
                <div className="text-[10px] text-[#39ff14]/50 tracking-widest">SPEED</div>
                <div className="text-2xl font-bold leading-none">
                  {derived.speed !== null ? derived.speed : <span className="text-[#39ff14]/20">—</span>}
                </div>
                <div className="text-[10px] text-[#39ff14]/60">KM/H</div>
              </div>
              <div>
                <div className="text-[10px] text-[#39ff14]/50 tracking-widest">RPM</div>
                <div className="text-2xl font-bold leading-none">
                  {derived.rpm !== null ? derived.rpm : <span className="text-[#39ff14]/20">—</span>}
                </div>
                <div className="text-[10px] text-[#39ff14]/60">ENGINE</div>
              </div>
              <div>
                <div className="text-[10px] text-[#39ff14]/50 tracking-widest">GEAR</div>
                <div className="text-2xl font-bold leading-none">{derived.gear}</div>
                <div className="text-[10px] text-[#39ff14]/60">CURRENT</div>
              </div>
            </div>
            <div className="w-full h-1 bg-[#39ff14]/10 mb-2">
              <div
                className="h-full bg-[#39ff14] transition-all"
                style={{ width: derived.rpm !== null ? `${Math.min(100, (derived.rpm / 3000) * 100)}%` : "0%" }}
              />
            </div>
            <div className="flex items-center justify-between pt-1 border-t border-[#39ff14]/20 text-sm">
              <span className="text-[#39ff14]/60 flex items-center gap-1">
                <Fuel size={12} /> FUEL
              </span>
              <span className={`font-bold ${derived.fuelLow ? "text-red-400 animate-pulse" : "text-[#39ff14]"}`}>
                {derived.fuel !== null ? `${derived.fuel}%` : "—"}
              </span>
            </div>
            <div className="w-full h-1.5 bg-[#39ff14]/10 mt-1 border border-[#39ff14]/20">
              <div
                className={`h-full transition-all ${
                  derived.fuel === null ? "" :
                  derived.fuel > 40 ? "bg-[#39ff14]" :
                  derived.fuel > 15 ? "bg-yellow-400" : "bg-red-500 animate-pulse"
                }`}
                style={{ width: derived.fuel !== null ? `${derived.fuel}%` : "0%" }}
              />
            </div>
          </div>

          {/* WEAPONS / WARNINGS */}
          <div className="border-2 border-[#39ff14]/30 bg-[#0a1a0a] p-3 flex flex-col">
            <div className="flex items-center gap-2 border-b border-[#39ff14]/30 pb-1 mb-2">
              <AlertTriangle size={16} className="text-[#39ff14]" />
              <h2 className="text-sm tracking-widest font-bold">WARNINGS</h2>
            </div>
            <div className="grid grid-cols-2 gap-2 mb-2">
              <Warning label="ENGINE FIRE" active={derived.fire}      icon={Flame} />
              <Warning label="OVERSPEED"   active={derived.overspeed} icon={Gauge} />
              <Warning label="LWS"         active={derived.lws}       icon={Radio} />
              <Warning label="IRCM"        active={derived.ircm}      icon={Eye} />
              <Warning label="AMMO LOW"    active={derived.ammoLow}   icon={Crosshair} />
              <Warning label="FUEL LOW"    active={derived.fuelLow}   icon={Fuel} />
            </div>
            <div className="mt-auto pt-1 border-t border-[#39ff14]/20 text-sm flex justify-between">
              <span className="text-[#39ff14]/60 flex items-center gap-1">
                <TargetIcon size={12} /> AMMO
              </span>
              <span className={`font-bold ${derived.ammoLow ? "text-red-400 animate-pulse" : "text-[#39ff14]"}`}>
                {derived.ammo !== undefined ? derived.ammo : "—"}
              </span>
            </div>
          </div>
        </div>

        {/* Row 2 — Event feed */}
        <div className="flex-1 min-h-0">
          <EventFeed className="h-full overflow-hidden" limit={10} />
        </div>

        {/* Bottom Bar */}
        <div className="border-t-2 border-[#39ff14]/50 pt-2 mt-3 flex justify-between shrink-0 text-sm tracking-widest text-[#39ff14]/80">
          <div>
            {bridgeOnline ? "BRIDGE: READY" : "BRIDGE: START wt_web_bridge.py"}
          </div>
          <div className="flex gap-4">
            <span className={derived.ammoLow ? "text-red-400 animate-pulse" : ""}>
              AMMO: {derived.ammo !== undefined ? derived.ammo : "—"}
            </span>
            <span>|</span>
            <span className={
              derived.crewCurrent !== undefined && derived.crewTotal !== undefined && derived.crewCurrent < derived.crewTotal
                ? "text-red-400" : ""
            }>
              CREW: {derived.crewCurrent !== undefined && derived.crewTotal !== undefined ? `${derived.crewCurrent}/${derived.crewTotal}` : "—"}
            </span>
            <span>|</span>
            <span className={derived.stabOn ? "text-[#39ff14]" : "text-[#39ff14]/40"}>
              STAB: {derived.wtOnline ? (derived.stabOn ? "ON" : "OFF") : "—"}
            </span>
          </div>
        </div>
      </div>
    </div>
  );
}
