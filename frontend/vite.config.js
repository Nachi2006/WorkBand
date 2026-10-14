import { defineConfig, loadEnv } from 'vite';
import react from '@vitejs/plugin-react';

// https://vitejs.dev/config/
export default defineConfig(({ mode }) => {
  // Load frontend .env so we can reference VITE_* vars here at config time
  const env = loadEnv(mode, process.cwd(), '');

  const backendUrl = env.VITE_API_URL || 'http://127.0.0.1:8001';
  const frontendPort = parseInt(env.VITE_PORT || '5173', 10);

  return {
    plugins: [react()],

    server: {
      port: frontendPort,
      host: '0.0.0.0',
      // Dev-server proxy: forwards /api/* and /ws/* to the backend
      // so you never hardcode the backend URL in fetch() calls —
      // only VITE_API_URL in the .env matters.
      proxy: {
        '/api': {
          target: backendUrl,
          changeOrigin: true,
          secure: false,
        },
        '/ws': {
          target: backendUrl.replace(/^http/, 'ws'),
          changeOrigin: true,
          ws: true,
        },
      },
    },

    build: {
      outDir: 'dist',
      sourcemap: false,
    },
  };
});
