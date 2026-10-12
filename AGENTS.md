# Evener Playdate client

Address the human partner as Jesse. Keep changes small and preserve working
behavior. Read `README.md` and `docs/client.md` before changing the client.

- The repository owns the native Playdate client, enrollment helper and client
  qualification. Generic Hub, browser and phone pairing belongs in Evener.
- Work on a `codex/` branch in an isolated worktree. Preserve unrelated changes
  and commit meaningful increments. Never bypass pre-commit hooks.
- Keep documentation evergreen with behavior. `docs/device-pairing.md` is a
  reference to the Evener-owned contract, not a client-specific server API.
- Keep `Source/config.json`, `Source/tailnet.json`, enrollment state, generated
  bundles and diagnostic captures out of Git. Never print private credentials.
- One runtime owns each native Tailscale identity. Do not run an enrollment helper
  or host probe with the device's active identity.
- Preserve vendor notices and provenance. Avoid whitespace-only vendor edits.
- Jesse permits skipping new and heavyweight tests for this prototype. Run the
  relevant existing checks and build both targets. Device behavior and live
  service claims require end-to-end evidence at the exercised boundary.
- Never make default verification issue provider requests or enroll a node.
- Follow the fixed firmware stack and asynchronous SDK ownership contracts in
  the README. Find root causes before changing recovery behavior.
