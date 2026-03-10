/**
 * War Thunder Web Bridge API
 * Connects the React interface to the local Python bridge (wt_web_bridge.py)
 * which runs on http://localhost:8112.
 *
 * The bridge proxies the WT localhost API (http://127.0.0.1:8111/) and
 * forwards keystroke commands to the game via the `keyboard` Python module.
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
  /** Bridge error message when WT is offline */
  error?: string;
}

// ─── Map types (from WT /map_obj.json + /map_info.json) ───────────────────────

export interface WtMapObject {
  /** Normalised X position [0, 1] */
  x: number;
  /** Normalised Y position [0, 1] */
  y: number;
  /** WT object type string, e.g. "capture_zone", "airfield", "tank" */
  type?: string;
  /** Hex colour string, e.g. "#ff0000" */
  color?: string;
  /** Icon identifier */
  icon?: string;
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

// ─── Entity classification (mirrors color_to_type() in wt_telemetry.py) ──────

export type EntityKind = "ally" | "enemy" | "objective" | "airfield" | "unknown";

export function classifyEntity(color: string, type: string): EntityKind {
  const t = (type ?? "").toLowerCase();
  if (t.includes("airfield")) return "airfield";
  if (
    t.includes("capture_zone") ||
    t.includes("bomb_point") ||
    t.includes("respawn_base")
  )
    return "objective";

  const hex = (color ?? "").replace("#", "").toLowerCase();
  if (hex.length < 6) return "unknown";
  const r = parseInt(hex.slice(0, 2), 16);
  const g = parseInt(hex.slice(2, 4), 16);
  const b = parseInt(hex.slice(4, 6), 16);

  if (r > 150 && r > g * 1.5 && r > b * 1.5) return "enemy";
  if (g > 100 && g > r * 1.2) return "ally";
  if (r > 180 && g > 180 && b < 80) return "objective";
  return "unknown";
}

// ─── Helpers ───────────────────────────────────────────────────────────────────

/** Extracts the short vehicle name from the WT type path. */
export function extractVehicleName(typePath?: string): string {
  if (!typePath) return "UNKNOWN";
  return typePath.split("/").pop()?.toUpperCase().replace(/_/g, " ") ?? "UNKNOWN";
}

// ─── Bridge requests ──────────────────────────────────────────────────────────

/** Sends a keystroke to War Thunder via the bridge. */
export async function sendCommand(key: string): Promise<void> {
  await fetch(`${BRIDGE_URL}/api/command`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ key }),
  });
}

/** Checks if the bridge is reachable. */
export async function checkBridgeHealth(): Promise<boolean> {
  try {
    const res = await fetch(`${BRIDGE_URL}/api/health`, { signal: AbortSignal.timeout(1000) });
    return res.ok;
  } catch {
    return false;
  }
}

/** Fetches current telemetry. Returns null when bridge is offline. */
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

/** Fetches current map data. Returns null when bridge is offline. */
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

/** Returns the URL of the WT map image streamed through the bridge. */
export function mapImageUrl(): string {
  return `${BRIDGE_URL}/api/map/image`;
}
