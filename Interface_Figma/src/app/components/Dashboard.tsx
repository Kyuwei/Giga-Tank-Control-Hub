import React, { useState, useEffect, useCallback } from "react";
import { useNavigate } from "react-router";
import { 
  CloudFog, 
  Power, 
  Crosshair, 
  Flame, 
  Map as MapIcon, 
  Binoculars, 
  Eye, 
  Ruler, 
  Target, 
  Cable,
  Activity,
  Wifi,
  WifiOff
} from "lucide-react";
import { MfdButton } from "./MfdButton";
import { sendCommand, checkBridgeHealth } from "../api";

export function Dashboard() {
  const navigate = useNavigate();
  const [time, setTime] = useState(new Date().toLocaleTimeString('en-US', { hour12: false }));
  const [bridgeOnline, setBridgeOnline] = useState<boolean | null>(null);

  useEffect(() => {
    const timer = setInterval(() => {
      setTime(new Date().toLocaleTimeString('en-US', { hour12: false }));
    }, 1000);
    return () => clearInterval(timer);
  }, []);

  // Poll bridge health every 3 s
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

  const handleCommand = useCallback(async (key: string) => {
    if (!bridgeOnline) {
      console.warn(`Bridge offline — cannot send key "${key}"`);
      return;
    }
    try {
      await sendCommand(key);
    } catch (err) {
      console.error("sendCommand error:", err);
    }
  }, [bridgeOnline]);

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
          <div className="flex gap-6 text-xl">
            <div className="flex flex-col">
              <span className="text-[#39ff14]/60 text-sm">SYSTEM</span>
              <span className="animate-pulse text-[#39ff14]">ONLINE</span>
            </div>
            <div className="flex flex-col">
              <span className="text-[#39ff14]/60 text-sm">BRIDGE</span>
              {bridgeOnline === null ? (
                <span className="text-[#39ff14]/50">INIT...</span>
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
          </div>
          
          <div className="flex-1 flex justify-center gap-4">
            <button 
              className="px-6 py-2 border-2 border-[#39ff14] text-[#39ff14] hover:bg-[#39ff14]/20 active:bg-[#39ff14]/40 transition-colors shadow-[0_0_15px_rgba(57,255,20,0.3)] hover:shadow-[0_0_20px_rgba(57,255,20,0.5)] flex items-center gap-2 font-bold tracking-widest"
              onClick={() => navigate('/map')}
            >
              <MapIcon size={20} />
              TACTICAL MAP
            </button>
            <button 
              className="px-6 py-2 border-2 border-[#39ff14] text-[#39ff14] hover:bg-[#39ff14]/20 active:bg-[#39ff14]/40 transition-colors shadow-[0_0_15px_rgba(57,255,20,0.3)] hover:shadow-[0_0_20px_rgba(57,255,20,0.5)] flex items-center gap-2 font-bold tracking-widest"
              onClick={() => navigate('/status')}
            >
              <Activity size={20} />
              SYS STATUS
            </button>
          </div>
          
          <div className="text-right">
            <div className="text-[#39ff14]/60 text-sm tracking-widest">M1A2 ABRAMS MFD // TACTICAL HUB</div>
            <div className="text-2xl tracking-wider">{time}</div>
          </div>
        </div>

        {/* Main Area */}
        <div className="flex-1 flex gap-2 md:gap-4 overflow-hidden mt-4">

          {/* Main Grid */}
          <div className="flex-1 grid grid-cols-2 sm:grid-cols-3 md:grid-cols-5 gap-2 md:gap-4 overflow-auto pb-2 pr-2">
          <MfdButton 
            icon={<Power size={48} />} 
            label="ENGINE" 
            subLabel="START/STOP (I)"
            onClick={() => handleCommand('I')} 
          />
          <MfdButton 
            icon={<CloudFog size={48} />} 
            label="SMOKE" 
            subLabel="DEPLOY (G)"
            onClick={() => handleCommand('G')} 
            alert
          />
          <MfdButton 
            icon={<Crosshair size={48} />} 
            label="ARTILLERY" 
            subLabel="CALL (5)"
            onClick={() => handleCommand('5')} 
            alert
          />
          <MfdButton 
            icon={<Flame size={48} />} 
            label="EXTINGUISHER" 
            subLabel="FPE (6)"
            onClick={() => handleCommand('6')} 
          />
          <MfdButton 
            icon={<Eye size={48} />} 
            label="THERMAL / NVD" 
            subLabel="TOGGLE (N)"
            onClick={() => handleCommand('N')} 
          />
          <MfdButton 
            icon={<Binoculars size={48} />} 
            label="BINOCULARS" 
            subLabel="VIEW (B)"
            onClick={() => handleCommand('B')} 
          />
          <MfdButton 
            icon={<Ruler size={48} />} 
            label="RANGEFINDER" 
            subLabel="MEASURE (R)"
            onClick={() => handleCommand('R')} 
          />
          <MfdButton 
            icon={<Target size={48} />} 
            label="TRACK TARGET" 
            subLabel="LOCK (X)"
            onClick={() => handleCommand('X')} 
          />
          <MfdButton 
            icon={<Cable size={48} />} 
            label="TOW CABLE" 
            subLabel="ATTACH (0)"
            onClick={() => handleCommand('0')} 
          />
          <MfdButton 
            icon={<MapIcon size={48} />} 
            label="ARTY STRIKE" 
            subLabel="COORDS (M+CLK)"
            onClick={() => handleCommand('M')} 
          />
        </div>
        </div>

        {/* Bottom Bar */}
        <div className="border-t-2 border-[#39ff14]/50 pt-2 mt-4 flex justify-between shrink-0 text-sm tracking-widest text-[#39ff14]/80">
          <div>
            {bridgeOnline
              ? "BRIDGE: READY"
              : "BRIDGE: START wt_web_bridge.py"}
          </div>
          <div className="flex gap-4">
            <div className="px-4 border border-[#39ff14]/30 bg-[#39ff14]/10">L1</div>
            <div className="px-4 border border-[#39ff14]/30 bg-[#39ff14]/10">L2</div>
            <div className="px-4 border border-[#39ff14]/30 bg-[#39ff14]/10">L3</div>
          </div>
          <div>ARMAMENT: HOT</div>
        </div>
      </div>
    </div>
  );
}
