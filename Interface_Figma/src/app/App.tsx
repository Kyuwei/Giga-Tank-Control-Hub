import { RouterProvider } from 'react-router';
import { router } from './routes';

export default function App() {
  return (
    <div style={{ fontFamily: "'Share Tech Mono', monospace" }} className="w-full h-full min-h-screen bg-black text-[#39ff14] overflow-hidden">
      <RouterProvider router={router} />
    </div>
  );
}
