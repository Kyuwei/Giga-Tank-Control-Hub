import { createBrowserRouter } from "react-router";
import { Dashboard } from "./components/Dashboard";
import { TacticalMap } from "./components/TacticalMap";
import { VehicleStatus } from "./components/VehicleStatus";

export const router = createBrowserRouter([
  {
    path: "/",
    Component: Dashboard,
  },
  {
    path: "/map",
    Component: TacticalMap,
  },
  {
    path: "/status",
    Component: VehicleStatus,
  }
]);
