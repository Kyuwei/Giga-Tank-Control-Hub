import React from "react";
import { ArrowLeft, Target, ShieldAlert, Navigation, Flag } from "lucide-react";
import { useNavigate } from "react-router";

export function TacticalMap() {
  const navigate = useNavigate();

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
              <span className="animate-pulse text-[#39ff14]">CONNECTED</span>
            </div>
          </div>
          
          <div className="text-right">
            <div className="text-[#39ff14]/60 text-sm tracking-widest">TACTICAL GRID // AREA 51</div>
            <div className="text-xl tracking-wider">COORD: 45N 12E</div>
          </div>
        </div>

        {/* Map Area */}
        <div className="flex-1 relative border-2 border-[#39ff14]/30 overflow-hidden bg-[#0a1a0a] flex items-center justify-center">
          {/* Grid lines */}
          <div className="absolute inset-0 bg-[linear-gradient(rgba(57,255,20,0.1)_1px,transparent_1px),linear-gradient(90deg,rgba(57,255,20,0.1)_1px,transparent_1px)] bg-[size:40px_40px]"></div>
          
          {/* Radar Sweep */}
          <div className="absolute w-[800px] h-[800px] rounded-full border border-[#39ff14]/20 flex items-center justify-center">
            <div className="w-[600px] h-[600px] rounded-full border border-[#39ff14]/20 flex items-center justify-center">
              <div className="w-[400px] h-[400px] rounded-full border border-[#39ff14]/20 flex items-center justify-center">
                <div className="w-[200px] h-[200px] rounded-full border border-[#39ff14]/20"></div>
              </div>
            </div>
            {/* Sweep line animation */}
            <div className="absolute top-1/2 left-1/2 w-[400px] h-[2px] bg-gradient-to-r from-transparent via-[#39ff14]/50 to-[#39ff14] origin-left animate-[spin_4s_linear_infinite]"></div>
          </div>

          {/* Map Content (Simulated Topo/Points of interest) */}
          <div className="relative w-full h-full p-8 z-10">
            {/* Friendly Marker */}
            <div className="absolute top-1/2 left-1/2 transform -translate-x-1/2 -translate-y-1/2 flex flex-col items-center z-20">
              <Navigation className="text-[#39ff14] fill-[#39ff14]/30 w-8 h-8 -rotate-45 drop-shadow-[0_0_8px_rgba(57,255,20,0.8)]" />
              <span className="text-xs mt-1 bg-black/50 px-1 border border-[#39ff14]/50 font-bold">M1A2</span>
            </div>

            {/* Spawns */}
            {/* Blue Spawns (Bottom Left / Bottom Right) */}
            <div className="absolute bottom-[10%] left-[10%] flex flex-col items-center">
              <Flag className="text-[#39ff14] w-6 h-6 fill-[#39ff14]/30" />
              <span className="text-[10px] mt-1 text-[#39ff14] bg-black/50 px-1 border border-[#39ff14]/50">SPAWN 1</span>
            </div>
            <div className="absolute bottom-[10%] right-[20%] flex flex-col items-center">
              <Flag className="text-[#39ff14] w-6 h-6 fill-[#39ff14]/30" />
              <span className="text-[10px] mt-1 text-[#39ff14] bg-black/50 px-1 border border-[#39ff14]/50">SPAWN 2</span>
            </div>

            {/* Red Spawns (Top Left / Top Right) */}
            <div className="absolute top-[10%] left-[15%] flex flex-col items-center opacity-80">
              <Flag className="text-red-500 w-6 h-6 fill-red-500/30" />
              <span className="text-[10px] mt-1 text-red-500 bg-black/50 px-1 border border-red-500/50">SPAWN 1</span>
            </div>
            <div className="absolute top-[10%] right-[10%] flex flex-col items-center opacity-80">
              <Flag className="text-red-500 w-6 h-6 fill-red-500/30" />
              <span className="text-[10px] mt-1 text-red-500 bg-black/50 px-1 border border-red-500/50">SPAWN 2</span>
            </div>

            {/* Objectives A, B, C, D */}
            {/* A Point - Mid Left */}
            <div className="absolute top-[40%] left-[30%] flex flex-col items-center">
              <Target className="text-white w-8 h-8 opacity-70" />
              <span className="text-sm mt-1 text-white font-bold bg-black/50 px-2 py-0.5 border-2 border-white/50">A</span>
            </div>

            {/* B Point - Center */}
            <div className="absolute top-[45%] left-[50%] flex flex-col items-center transform -translate-x-1/2">
              <Target className="text-red-500 w-8 h-8 animate-pulse" />
              <span className="text-sm mt-1 text-red-500 font-bold bg-black/50 px-2 py-0.5 border-2 border-red-500/50">B</span>
            </div>

            {/* C Point - Mid Right */}
            <div className="absolute top-[35%] right-[25%] flex flex-col items-center">
              <Target className="text-blue-400 w-8 h-8" />
              <span className="text-sm mt-1 text-blue-400 font-bold bg-black/50 px-2 py-0.5 border-2 border-blue-400/50">C</span>
            </div>

            {/* D Point - Bottom Center */}
            <div className="absolute top-[65%] left-[45%] flex flex-col items-center">
              <Target className="text-white w-8 h-8 opacity-70" />
              <span className="text-sm mt-1 text-white font-bold bg-black/50 px-2 py-0.5 border-2 border-white/50">D</span>
            </div>

            {/* Enemy Markers */}
            <div className="absolute top-[38%] left-[45%] flex flex-col items-center animate-pulse">
              <ShieldAlert className="text-red-500 w-5 h-5" />
              <span className="text-[10px] mt-1 text-red-500 bg-black/80 px-1 border border-red-500/50">T-80BVM</span>
            </div>
            
            {/* Simulated Terrain Contours */}
            <svg className="absolute inset-0 w-full h-full opacity-20 pointer-events-none" viewBox="0 0 800 600" preserveAspectRatio="none">
              <path d="M 0 200 Q 200 150 400 300 T 800 100" fill="none" stroke="#39ff14" strokeWidth="2" />
              <path d="M 0 250 Q 200 200 400 350 T 800 150" fill="none" stroke="#39ff14" strokeWidth="2" />
              <path d="M 0 400 Q 300 450 500 300 T 800 400" fill="none" stroke="#39ff14" strokeWidth="2" />
            </svg>
          </div>
        </div>

        {/* Bottom Bar */}
        <div className="border-t-2 border-[#39ff14]/50 pt-2 mt-4 flex justify-between shrink-0 text-sm tracking-widest text-[#39ff14]/80">
          <div>ZOOM: 2.5X</div>
          <div>MODE: TOPO / THERMAL</div>
          <div>GPS: LOCK (9 SATS)</div>
        </div>
      </div>
    </div>
  );
}
