import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  base: "/dash-assets/",
  server: {
    port: 5174,
    strictPort: true,
  },
});
