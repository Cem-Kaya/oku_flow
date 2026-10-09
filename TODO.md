- [x] Touchpad two-finger pan now updates the zoom center while zoom is active (handled via precision wheel gestures without modifiers).
- [x] Added a "Show Focus Point" debug checkbox that draws a red marker at the live zoom center, keeping it in sync with cursor-driven updates.
- [ ] Switching cameras can trigger a crash (likely during CUDA surface reinitialization). Investigate once the blur performance work is stable.
- [ ] Expand GPU logging even further (capture cudaExternalMemory import parameters, add optional break-on-fail hooks) while porting remaining pipeline stages to CUDA.
- [ ] Prototype a lightweight vision-language model overlay that can describe
      magnified regions and provide contextual hints for visually impaired
      users.
