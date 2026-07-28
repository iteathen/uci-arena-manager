import { defineConfig } from 'vitest/config'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    host: '127.0.0.1',
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:47831',
        ws: true,
      },
    },
  },
  build: {
    target: 'es2022',
    sourcemap: true,
    outDir: 'dist',
    chunkSizeWarningLimit: 500,
    rollupOptions: {
      output: {
        manualChunks(id) {
          const moduleId = id.replaceAll('\\', '/')
          if (
            moduleId.includes('/node_modules/@mantine/core/')
            || moduleId.includes('/node_modules/@mantine/hooks/')
          ) {
            return 'mantine'
          }
          if (moduleId.includes('/node_modules/@tabler/icons-react/')) {
            return 'icons'
          }
          if (
            moduleId.includes('/node_modules/@tanstack/react-query/')
            || moduleId.includes('/node_modules/@tanstack/react-table/')
            || moduleId.includes('/node_modules/@tanstack/react-virtual/')
          ) {
            return 'tanstack'
          }
          return undefined
        },
      },
    },
  },
  test: {
    environment: 'jsdom',
  },
})
