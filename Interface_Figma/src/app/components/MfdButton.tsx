import React, { ReactNode } from 'react';

interface MfdButtonProps {
  icon: ReactNode;
  label: string;
  subLabel?: string;
  onClick: () => void;
  alert?: boolean;
}

export function MfdButton({ icon, label, subLabel, onClick, alert }: MfdButtonProps) {
  return (
    <button
      onClick={onClick}
      className={`relative group flex flex-col items-center justify-center p-4 border-2 transition-all duration-150 overflow-hidden outline-none touch-none
        ${alert 
          ? 'border-red-500 text-red-500 hover:bg-red-500/20 active:bg-red-500/40 shadow-[0_0_10px_rgba(239,68,68,0.3)]' 
          : 'border-[#39ff14] text-[#39ff14] hover:bg-[#39ff14]/20 active:bg-[#39ff14]/40 shadow-[0_0_10px_rgba(57,255,20,0.3)]'
        }
      `}
      style={{
        boxShadow: alert ? 'inset 0 0 15px rgba(239, 68, 68, 0.2)' : 'inset 0 0 15px rgba(57, 255, 20, 0.2)',
      }}
    >
      {/* Corner accents */}
      <div className={`absolute top-0 left-0 w-2 h-2 border-t-2 border-l-2 ${alert ? 'border-red-500' : 'border-[#39ff14]'}`} />
      <div className={`absolute top-0 right-0 w-2 h-2 border-t-2 border-r-2 ${alert ? 'border-red-500' : 'border-[#39ff14]'}`} />
      <div className={`absolute bottom-0 left-0 w-2 h-2 border-b-2 border-l-2 ${alert ? 'border-red-500' : 'border-[#39ff14]'}`} />
      <div className={`absolute bottom-0 right-0 w-2 h-2 border-b-2 border-r-2 ${alert ? 'border-red-500' : 'border-[#39ff14]'}`} />

      {/* Button content */}
      <div className={`mb-1 sm:mb-2 md:mb-3 transform group-active:scale-95 transition-transform duration-75 ${alert ? 'drop-shadow-[0_0_8px_rgba(239,68,68,0.8)]' : 'drop-shadow-[0_0_8px_rgba(57,255,20,0.8)]'}`}>
        <div className="scale-75 sm:scale-90 md:scale-100">{icon}</div>
      </div>
      
      <div className="text-center w-full mt-auto mb-auto">
        <div className={`font-bold tracking-widest text-sm sm:text-base md:text-lg group-active:scale-95 transition-transform duration-75 ${alert ? 'text-red-500 drop-shadow-[0_0_5px_rgba(239,68,68,0.8)]' : 'text-[#39ff14] drop-shadow-[0_0_5px_rgba(57,255,20,0.8)]'}`}>
          {label}
        </div>
        {subLabel && (
          <div className={`text-[10px] sm:text-xs mt-0.5 sm:mt-1 tracking-wider group-active:scale-95 transition-transform duration-75 ${alert ? 'text-red-500/70' : 'text-[#39ff14]/70'}`}>
            [{subLabel}]
          </div>
        )}
      </div>

      {/* Hover/Active overlay for tech feel */}
      <div className="absolute inset-0 bg-gradient-to-b from-transparent to-black/30 pointer-events-none" />
    </button>
  );
}
