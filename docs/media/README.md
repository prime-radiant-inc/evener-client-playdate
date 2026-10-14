# Evener for Playdate films

The repository README embeds both films using GitHub video attachments. Each
attachment URL occupies its own Markdown paragraph so GitHub renders an inline
player with audio controls. The original MP4s remain in this directory.

To replace a film, upload the reviewed MP4 as an attachment to its pull request
(for example, with `gh pr edit --attach PATH`), verify the uploaded bytes match
the repository file, then update both its standalone URL and text link in the
README. Verify playback on the rendered README before merging; a normal link to
a repository MP4 or a linked poster does not create an inline player.

## Recorded onboarding walkthrough

The [onboarding walkthrough](evener-playdate-onboarding.mp4) is 2 minutes
34 seconds at 1920 × 1080 and 30 fps, with stereo AAC, selectable English
captions and 15 chapters. Its [poster](onboarding-poster.png) and
[captions](evener-playdate-onboarding.en.srt) are also available separately.

It explains private building and USB sideloading, then uses actual Playdate
and Hub captures to teach receiving setup sound, comparing the complete code,
Hub approval, saving, and voice/review/send/reply. Its setup sound is the actual
room recording from the production receiver take, with narration kept clear of
both audible blocks.

The phone's Tailscale sign-in and approval were not filmed. That step is an
explicitly labeled instruction card. The setup sound and approval chapters come
from different recorded takes, also labeled in the film. Waits are edited;
narration and the original dictated demonstration speech are synthesized. The
film is an instructional edit and does not claim continuous enrollment footage
or a connection-speed measurement.

[Editable source](../../tools/onboarding/README.md) includes the selected
capture clips and audio, captions, chapters, renderer and
[provenance](../../tools/onboarding/PROVENANCE.json). Full raw recordings,
unrelated sessions, private configuration, enrollment identities and mobile
connection QR material are excluded.

## A little closer

[A little closer](evener-playdate-promo.mp4) is a 58-second product tour at
1920 × 1080 and 60 frames per second, with stereo AAC audio and the official
Prime Radiant logo. [The poster](poster.png) is also available separately.

The film combines actual physical Playdate screen captures with an illustrated
device, animated typography and an original synthesized score. Recording,
review, sending and the assistant's reply were captured in the dedicated
demonstration conversation. The sound-pairing scene shows the receiver waiting
for an invitation. The film is an edited montage, not a continuous onboarding
recording or a measurement of connection latency.

The short chirps in the music are musical effects. They are not the Hub's
encoded setup sound.

[Production source](../../tools/promo/README.md), including selected capture
clips and the score generator, can reproduce the film without credentials,
a Hub or a connected Playdate. [Capture provenance](../../tools/promo/inputs/PROVENANCE.json)
records the source ranges and transformations. Full source recordings and
configured application bundles are kept outside this public repository.

The Prime Radiant logo comes from the
[official brand asset](https://primeradiant.com/brand/logo.png). Playdate is
Panic's handheld; the device housing in this film is an illustration.
