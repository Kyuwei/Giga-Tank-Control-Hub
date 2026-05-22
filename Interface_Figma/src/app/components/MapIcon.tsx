import { memo } from "react";
import {
  Square, Plane, Flag, Tent, Target, RotateCcw, Circle,
} from "lucide-react";
import type { EntityKind } from "../api";

const COLOUR: Record<EntityKind, string> = {
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

interface Props {
  kind:     EntityKind;
  /** percent-position inside the map container (0..100) */
  left:     number;
  top:      number;
  /** degrees, 0 = pointing along +x (used only for vehicles) */
  rotation: number;
  /** the raw WT type string, surfaced as a tooltip */
  label?:   string;
}

function chooseIcon(kind: EntityKind) {
  switch (kind) {
    case "tank_ally":
    case "tank_enemy":      return Square;
    case "aircraft_ally":
    case "aircraft_enemy":  return Plane;
    case "objective":       return Flag;
    case "airfield":        return Tent;
    case "bomb_point":      return Target;
    case "respawn":         return RotateCcw;
    default:                return Circle;
  }
}

const SIZES: Record<EntityKind, number> = {
  tank_ally:      14,
  tank_enemy:     14,
  aircraft_ally:  16,
  aircraft_enemy: 16,
  objective:      18,
  airfield:       20,
  bomb_point:     18,
  respawn:        18,
  unknown:        12,
};

const ROTATES: Partial<Record<EntityKind, boolean>> = {
  tank_ally:      true,
  tank_enemy:     true,
  aircraft_ally:  true,
  aircraft_enemy: true,
};

export const MapIcon = memo(function MapIcon({
  kind, left, top, rotation, label,
}: Props) {
  const Icon   = chooseIcon(kind);
  const colour = COLOUR[kind];
  const size   = SIZES[kind];
  const rotate = ROTATES[kind] ? rotation : 0;
  const blink  = kind === "tank_enemy" || kind === "aircraft_enemy" || kind === "bomb_point";

  return (
    <div
      className="absolute group"
      style={{ left: `${left}%`, top: `${top}%`, transform: "translate(-50%, -50%)" }}
    >
      <div
        className={`flex items-center justify-center rounded-full ${blink ? "animate-pulse" : ""}`}
        style={{
          width:           size + 8,
          height:          size + 8,
          backgroundColor: `${colour}22`,
          border:          `1px solid ${colour}88`,
          boxShadow:       `0 0 8px ${colour}77`,
        }}
      >
        <Icon
          size={size}
          color={colour}
          strokeWidth={2.4}
          style={{ transform: `rotate(${rotate}deg)`, transition: "transform 200ms linear" }}
        />
      </div>
      {label && (
        <div
          className="absolute left-1/2 -translate-x-1/2 -translate-y-2 top-full bg-black/80 px-2 py-1 text-[10px] text-[#39ff14] border border-[#39ff14]/30 tracking-wider opacity-0 group-hover:opacity-100 transition-opacity pointer-events-none whitespace-nowrap z-20"
        >
          {label}
        </div>
      )}
    </div>
  );
});
