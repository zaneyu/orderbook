import { defineConfig } from "vite";

// base './' -> all asset URLs are relative, so the built site works at any path
// (Cloudflare Pages root, a subfolder, or file://-style previews).
export default defineConfig({
  base: "./",
  build: { target: "es2020" },
});
