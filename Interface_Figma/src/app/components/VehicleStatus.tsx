import React from "react";
import { ArrowLeft, Users, Crosshair, Gauge, Activity, Shield } from "lucide-react";
import { useNavigate } from "react-router";

export function VehicleStatus() {
  const navigate = useNavigate();

  return (
    <div className="flex items-center justify-center w-full h-screen bg-black select-none p-2 sm:p-4">
      {/* CRT Container */}
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
              <span className="text-[#39ff14]/60 text-sm">DIAGNOSTICS</span>
              <span className="animate-pulse text-[#39ff14]">RUNNING</span>
            </div>
          </div>
          
          <div className="text-right">
            <div className="text-[#39ff14]/60 text-sm tracking-widest">VEHICLE TELEMETRY</div>
            <div className="text-xl tracking-wider">M1A2 SEP v3</div>
          </div>
        </div>

        {/* Main Content Area */}
        <div className="flex-1 grid grid-cols-1 md:grid-cols-2 gap-6 overflow-hidden z-10">
          
          {/* Left Column: Crew & Ammo */}
          <div className="flex flex-col gap-6">
            {/* Crew Status */}
            <div className="border-2 border-[#39ff14]/30 p-4 bg-[#0a1a0a]">
              <div className="flex items-center gap-2 mb-4 border-b border-[#39ff14]/30 pb-2">
                <Users className="text-[#39ff14]" size={20} />
                <h2 className="text-lg tracking-widest font-bold">CREW STATUS</h2>
              </div>
              <div className="grid grid-cols-2 gap-4">
                {[
                  { role: "COMMANDER", status: "OK", color: "text-[#39ff14]" },
                  { role: "GUNNER", status: "OK", color: "text-[#39ff14]" },
                  { role: "LOADER", status: "OK", color: "text-[#39ff14]" },
                  { role: "DRIVER", status: "OK", color: "text-[#39ff14]" }
                ].map((crew) => (
                  <div key={crew.role} className="flex justify-between items-center border border-[#39ff14]/20 p-2">
                    <span className="text-[#39ff14]/70 text-sm">{crew.role}</span>
                    <span className={`font-bold ${crew.color}`}>{crew.status}</span>
                  </div>
                ))}
              </div>
            </div>

            {/* Ammunition */}
            <div className="border-2 border-[#39ff14]/30 p-4 bg-[#0a1a0a] flex-1">
              <div className="flex items-center gap-2 mb-4 border-b border-[#39ff14]/30 pb-2">
                <Crosshair className="text-[#39ff14]" size={20} />
                <h2 className="text-lg tracking-widest font-bold">AMMUNITION</h2>
              </div>
              <div className="space-y-3">
                <div className="flex flex-col">
                  <div className="flex justify-between items-end mb-1">
                    <span className="text-sm">APFSDS (M829A4)</span>
                    <span className="text-xl font-bold">22</span>
                  </div>
                  <div className="w-full h-2 bg-[#39ff14]/20">
                    <div className="h-full bg-[#39ff14]" style={{ width: '55%' }}></div>
                  </div>
                </div>
                <div className="flex flex-col">
                  <div className="flex justify-between items-end mb-1">
                    <span className="text-sm">HEAT-MP-T (M830A1)</span>
                    <span className="text-xl font-bold">14</span>
                  </div>
                  <div className="w-full h-2 bg-[#39ff14]/20">
                    <div className="h-full bg-[#39ff14]" style={{ width: '35%' }}></div>
                  </div>
                </div>
                <div className="flex justify-between items-center border-t border-[#39ff14]/20 pt-2 mt-4 text-[#39ff14]/70">
                  <span>COAX 7.62mm</span>
                  <span className="font-mono">8500 / 10000</span>
                </div>
                <div className="flex justify-between items-center text-[#39ff14]/70">
                  <span>.50 CAL (CROWS)</span>
                  <span className="font-mono">800 / 900</span>
                </div>
              </div>
            </div>
          </div>

          {/* Right Column: Telemetry & Damage */}
          <div className="flex flex-col gap-6">
            {/* Live Telemetry */}
            <div className="border-2 border-[#39ff14]/30 p-4 bg-[#0a1a0a]">
              <div className="flex items-center gap-2 mb-4 border-b border-[#39ff14]/30 pb-2">
                <Gauge className="text-[#39ff14]" size={20} />
                <h2 className="text-lg tracking-widest font-bold">DRIVETRAIN</h2>
              </div>
              <div className="grid grid-cols-2 gap-4">
                <div className="border border-[#39ff14]/20 p-4 flex flex-col items-center justify-center relative">
                  <span className="text-[#39ff14]/50 text-xs absolute top-2 left-2">SPEED</span>
                  <span className="text-4xl font-bold mt-2">42</span>
                  <span className="text-xs text-[#39ff14]/70">KM/H</span>
                </div>
                <div className="border border-[#39ff14]/20 p-4 flex flex-col items-center justify-center relative">
                  <span className="text-[#39ff14]/50 text-xs absolute top-2 left-2">RPM</span>
                  <span className="text-4xl font-bold mt-2">1850</span>
                  <span className="text-xs text-[#39ff14]/70">TURBINE</span>
                </div>
                <div className="border border-[#39ff14]/20 p-4 flex flex-col items-center justify-center relative">
                  <span className="text-[#39ff14]/50 text-xs absolute top-2 left-2">GEAR</span>
                  <span className="text-4xl font-bold mt-2">4</span>
                  <span className="text-xs text-[#39ff14]/70">FORWARD</span>
                </div>
                <div className="border border-[#39ff14]/20 p-4 flex flex-col items-center justify-center relative">
                  <span className="text-[#39ff14]/50 text-xs absolute top-2 left-2">FUEL</span>
                  <span className="text-4xl font-bold mt-2">78</span>
                  <span className="text-xs text-[#39ff14]/70">% (JP-8)</span>
                </div>
              </div>
            </div>

            {/* Modules / Damage Panel */}
            <div className="border-2 border-[#39ff14]/30 p-4 bg-[#0a1a0a] flex-1">
              <div className="flex items-center gap-2 mb-4 border-b border-[#39ff14]/30 pb-2">
                <Shield className="text-[#39ff14]" size={20} />
                <h2 className="text-lg tracking-widest font-bold">MODULE INTEGRITY</h2>
              </div>
              <div className="space-y-2">
                {[
                  { name: "ENGINE", integrity: 100 },
                  { name: "TRANSMISSION", integrity: 100 },
                  { name: "TURRET DRIVE", integrity: 100 },
                  { name: "GUN BARREL", integrity: 100 },
                  { name: "TRACK L", integrity: 85 },
                  { name: "TRACK R", integrity: 90 },
                ].map((mod) => (
                  <div key={mod.name} className="flex items-center gap-4 text-sm">
                    <span className="w-32 text-[#39ff14]/70">{mod.name}</span>
                    <div className="flex-1 h-3 bg-[#39ff14]/20 border border-[#39ff14]/30">
                      <div 
                        className={`h-full ${mod.integrity > 50 ? 'bg-[#39ff14]' : 'bg-red-500'} transition-all`} 
                        style={{ width: `${mod.integrity}%` }}
                      ></div>
                    </div>
                    <span className="w-10 text-right font-mono">{mod.integrity}%</span>
                  </div>
                ))}
              </div>
            </div>
          </div>
        </div>

        {/* Bottom Bar */}
        <div className="border-t-2 border-[#39ff14]/50 pt-2 mt-4 flex justify-between shrink-0 text-sm tracking-widest text-[#39ff14]/80">
          <div className="flex items-center gap-2"><Activity size={16}/> SENSORS ACTIVE</div>
          <div>BMS: CONNECTED</div>
          <div>STAB: DUAL-AXIS ON</div>
        </div>
      </div>
    </div>
  );
}
