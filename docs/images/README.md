# README capture provenance

The gallery images were recorded from a physical Playdate through the official
Mirror app on 2026-10-14. Each PNG preserves one decoded 800 × 480 capture frame,
without cropping, resizing, retouching or replacement interface text.

Only the demonstration conversation and an empty setup receiver are included.
The selected frames contain no credentials, private session lists, login URLs or
active invitation data. Configured app bundles and full source recordings are
kept outside this public repository.

| Image | Source recording | Source time | Observed state |
| --- | --- | --- | --- |
| [Sessions](client-sessions.png) | `Playdate 2026-10-14_09:18:57.mp4` | 42.0 s | The `tmp` project contains only **Playdate client verification**, idle and connected. |
| [Conversation](client-conversation.png) | `device-02-voice-send.mp4` | 305.0 s | The successful demonstration exchange includes the assistant's **small screen big ideas** reply and an idle footer. |
| [Voice review](client-voice-review.png) | `Playdate 2026-10-14_09:11:56.mp4` | 40.0 s | Final text is ready for review. This separate duplicate take was never sent and was discarded afterward. |
| [Receive setup](client-pairing.png) | `Playdate 2026-10-14_09:24:00.mp4` | 25.0 s | The isolated setup-only app is listening at zero percent, before an invitation is played. |

The images document separate observed states. In particular, the receiver image
documents the start of setup; it does not establish a completed sound transfer
or Tailscale enrollment.

Mirror records sparse video updates and holds the last displayed screen between
them. Extraction expands those holds before selecting a time:

```sh
ffmpeg -i source.mp4 \
  -vf 'fps=30,trim=start=25,setpts=PTS-STARTPTS' \
  -frames:v 1 capture.png
```

Input-side `-ss` can select the next update instead of the held screen. The
gallery's selection times were checked against the visible decoded frames.

Source SHA-256 hashes:

- `Playdate 2026-10-14_09:18:57.mp4`:
  `e5c0bb05788997d111189faf7b589a97fa9a48fc49837af4650670f19c9ba57c`
- `device-02-voice-send.mp4`:
  `7d3bc086eadc04d27f118aeaf7f1483740b9b0bb82a3920f3ceb87b466e07a0e`
- `Playdate 2026-10-14_09:11:56.mp4`:
  `94830dce849bbcec1121b9ea755fc43da99e5d920342b70ffdb78efb1b7be505`
- `Playdate 2026-10-14_09:24:00.mp4`:
  `c43aba18eba361049ee5ee8cc2783f0a8884c6eb11f96e2f8bf482360a5a68db`
