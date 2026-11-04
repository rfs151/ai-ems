import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 开发时把 /api 和 /ws 转给本地云端服务
export default defineConfig({
  plugins: [vue()],
  server: {
    proxy: {
      '/api': 'http://127.0.0.1:8095',
      '/ws': { target: 'ws://127.0.0.1:8095', ws: true },
    },
  },
  build: { chunkSizeWarningLimit: 900 },
})
