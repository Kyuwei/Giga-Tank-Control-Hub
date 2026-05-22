/**
 * War Thunder Web Bridge API (V2 — read-only)
 * Connects the React interface to the local Python bridge (wt_web_bridge.py)
 * which runs on http://localhost:8112.
 *
 * The bridge proxies the WT localhost API (http://127.0.0.1:8111/) and serves
 * synthesised module-health + HUD event feeds. No keystroke injection — the
 * hub is fully passive.
 */

export const BRIDGE_URL = "http://localhost:8112";

// ─── Telemetry types (from WT /indicators endpoint) ───────────────────────────

export interface WtTelemetry {
  /** Speed in km/h */
  speed?: number;
  /** Engine RPM */
  rpm?: number;
  /** Current gear (number or "R" for reverse) */
  gear?: number | string;
  /** Main ammo count (first-stage rack) */
  first_stage_ammo?: number;
  /** Stabilizer active (1) or off (0) */
  stabilizer?: number;
  /** Number of alive crew members */
  crew_current?: number;
  /** Total crew members */
  crew_total?: number;
  /** Vehicle model path, e.g. "tankModels/us_m1a2_abrams" */
  type?: string;
  /** Vehicle category: "tank" | "aircraft" | "ship" */
  army?: string;
  /** Fuel percentage (0–100), if available */
  fuel?: number;
  /** Whether the engine is on fire */
  engine_on_fire?: boolean;
  /** Driver state (0 = alive, !=0 = down) */
  driver_state?: number;
  /** Gunner state (0 = alive, !=0 = down) */
  gunner_state?: number;
  /** Speed warning active */
  has_speed_warning?: number;
  /** Laser Warning System (-1 = absent) */
  lws?: number;
  /** IR counter-measures (-1 = absent) */
  ircm?: number;
  /** Bridge error message when WT is offline */
  error?: string;
}

// ─── Module health ────────────────────────────────────────────────────────────

export interface WtModuleHealth {
  ENGINE: number;
  TRANSMISSION: number;
  TURRET: number;
  BARREL: number;
  TRACK_L: number;
  TRACK_R: number;
}

// ─── HUD events ───────────────────────────────────────────────────────────────

export type WtEventKind = "kill" | "damage" | "alert";

export interface WtEvent {
  kind: WtEventKind;
  msg: string;
  ts: number;
  enemy: boolean;
}

// ─── Map types (from WT /map_obj.json + /map_info.json) ───────────────────────

export interface WtMapObject {
  /** Normalised X position [0, 1] */
  x: number;
  /** Normalised Y position [0, 1] */
  y: number;
  /** Tip X (for elongated objects like aircraft / runways) */
  ex?: number;
  ey?: number;
  /** WT object type string, e.g. "capture_zone", "airfield", "tank", "aircraft" */
  type?: string;
  /** Hex colour string, e.g. "#ff0000" */
  color?: string;
  /** Icon identifier */
  icon?: string;
  /** Set to 1 when the object is flashing (urgent threat / objective) */
  blink?: number;
}

export interface WtMapInfo {
  /** Human-readable map name */
  name?: string;
  /** Generation counter (increments on map change) */
  map_generation?: number;
}

export interface WtMapData {
  objects: WtMapObject[];
  info: WtMapInfo;
  error?: string;
}

// ─── Entity classification (mirrors classify_entity() in wt_telemetry.py) ─────

export type EntityKind =
  | "tank_ally" | "tank_enemy"
  | "aircraft_ally" | "aircraft_enemy"
  | "objective" | "airfield" | "bomb_point" | "respawn"
  | "unknown";

function dominantColour(hex: string): "red" | "green" | "yellow" | "other" {
  const h = (hex ?? "").replace("#", "").toLowerCase();
  if (h.length < 6) return "other";
  const r = parseInt(h.slice(0, 2), 16);
  const g = parseInt(h.slice(2, 4), 16);
  const b = parseInt(h.slice(4, 6), 16);
  if (Number.isNaN(r) || Number.isNaN(g) || Number.isNaN(b)) return "other";
  if (r > 150 && r > g * 1.5 && r > b * 1.5) return "red";
  if (g > 100 && g > r * 1.2) return "green";
  if (r > 180 && g > 180 && b < 80) return "yellow";
  return "other";
}

export function classifyEntity(obj: WtMapObject): EntityKind {
  const t = (obj.type ?? "").toLowerCase();
  const dom = dominantColour(obj.color ?? "");
  const isEnemy = dom === "red" || Boolean(obj.blink);

  if (t.includes("airfield"))       return "airfield";
  if (t.includes("bomb_point"))     return "bomb_point";
  if (t.includes("respawn_base"))   return "respawn";
  if (t.includes("capture_zone"))   return "objective";
  if (t.includes("aircraft") || t.includes("plane")) {
    return isEnemy ? "aircraft_enemy" : "aircraft_ally";
  }
  if (t.includes("tank") || t.includes("ground")) {
    return isEnemy ? "tank_enemy" : "tank_ally";
  }
  if (dom === "red")    return "tank_enemy";
  if (dom === "green")  return "tank_ally";
  if (dom === "yellow") return "objective";
  return "unknown";
}

/** Returns rotation in degrees [0, 360) from the ex/ey vector, or 0. */
export function entityRotation(obj: WtMapObject): number {
  const ex = obj.ex ?? 0;
  const ey = obj.ey ?? 0;
  if (Math.abs(ex) < 1e-6 && Math.abs(ey) < 1e-6) return 0;
  const deg = (Math.atan2(ey - obj.y, ex - obj.x) * 180) / Math.PI;
  return ((Math.round(deg) % 360) + 360) % 360;
}

// ─── Helpers ───────────────────────────────────────────────────────────────────

export function extractVehicleName(typePath?: string): string {
  if (!typePath) return "UNKNOWN";
  return typePath.split("/").pop()?.toUpperCase().replace(/_/g, " ") ?? "UNKNOWN";
}

// ─── Bridge requests ──────────────────────────────────────────────────────────

export async function checkBridgeHealth(): Promise<boolean> {
  try {
    const res = await fetch(`${BRIDGE_URL}/api/health`, { signal: AbortSignal.timeout(1000) });
    return res.ok;
  } catch {
    return false;
  }
}

export async function fetchTelemetry(): Promise<WtTelemetry | null> {
  try {
    const res = await fetch(`${BRIDGE_URL}/api/telemetry`, {
      signal: AbortSignal.timeout(800),
    });
    if (!res.ok) return null;
    return res.json();
  } catch {
    return null;
  }
}

export async function fetchMapData(): Promise<WtMapData | null> {
  try {
    const res = await fetch(`${BRIDGE_URL}/api/map`, {
      signal: AbortSignal.timeout(800),
    });
    if (!res.ok) return null;
    return res.json();
  } catch {
    return null;
  }
}

export async function fetchModules(): Promise<WtModuleHealth | null> {
  try {
    const res = await fetch(`${BRIDGE_URL}/api/modules`, {
      signal: AbortSignal.timeout(800),
    });
    if (!res.ok) return null;
    return res.json();
  } catch {
    return null;
  }
}

export async function fetchEvents(): Promise<WtEvent[]> {
  try {
    const res = await fetch(`${BRIDGE_URL}/api/events`, {
      signal: AbortSignal.timeout(800),
    });
    if (!res.ok) return [];
    const data = await res.json();
    return Array.isArray(data?.events) ? data.events : [];
  } catch {
    return [];
  }
}

export function mapImageUrl(): string {
  return `${BRIDGE_URL}/api/map/image`;
}
