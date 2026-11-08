This list is not filtered, there's no priority.

Status audited against the code on 2026-07-04. `[x]` = done, `[~]` = partial.

- [x] libcurl: http/3 support — h3.c/quic.c (RFC 9114 + QPACK), `curl --http3` wired
- [x] browser: css loading — external `<link rel=stylesheet>` fetched + applied (html.c collect_styles / batch fetch)
- [~] browser: 103 early hints + "Link" header support — 103 interim responses now handled (h1/h2 skip + merge hints; h3 merges); Link still only rel=stylesheet (no preload/preconnect)
- [x] browser: Accept-Ch header — clienthints.c: parses Accept-CH per origin (https-only, opt-in), sends Sec-CH-UA family + DPR + Viewport-Width on all transports (doc h1/h2/h3 + sub-resources)
- [x] browser: its own User-Agent instead of libcurl one — BR_UA sent on all paths (h1/h2/h3); libcurl core no longer injects a default UA (the `curl` tool now adds its own, `-A` to override)
- [x] brotli compression format — user/lib/brotli decoder + encoder, RFC 7932 dictionary
- [ ] programs: archive tool program to view archives (zip, tar, tar.gz, ...) — only single-file zip/gzip stream compress in term.c; no tar, no member listing
- [x] support brotli for browser [de]compression — advertises `br`, decodes via ENC_BR (h1 path; h2 intentionally sends bodies plain)
- [ ] Samba client — only smbd (server) exists; no outbound SMB client
- [ ] "Network" places in Files app — sidebar has Places + Devices only
- [ ] Phones MTP shown in Files app — `mtp` is a kernel recovery-console builtin only, not in the GUI
- [x] Volumes names shown in devices instead of devices names — Files shows vols[].label from FS volume label (SYS_VOLUMES)
- [~] NTFS support — read-only browse (list/stat/read + label), wired into mount table; no write (create/del/mkdir NULL)
- [ ] JPEG, MP4 encoder/decoder; WebP lossy (VP8) decode
      - JPEG: none
      - MP4: demuxer only (user/lib/mp4), no muxer/encoder; separate MPEG-1 codec exists but is elementary-stream, not MP4
      - WebP lossy VP8: not decoded (VP8L lossless only, returns WEBP_EUNSUP)
- [x] Dialog box GUI: sub-window linked to its parent — uwin.c now has parent/transient/modal windows (win_set_parent + UWIN_F_MODAL) over per-process multi-window; the compositor groups families in z, blocks modals, hides transients from taskbar/Alt+Tab, centers them. Reusable modal message box in user/lib/dialog (dialog_msg, icon+button flags); modeless rich dialogs are app-built (Files Properties). Consumers: editor unsaved-changes confirm, Files delete confirm
- [x] "Properties" dialog for files and folders in Files app (right-click item) — Properties is a modeless, app-built child window (name/location/type/size/path); Delete added to the same menu
- [~] Browser: view source, developer tools (page structure, Network reqs) — devtools panel full (Elements/Styles/Console/Network/App, F12); raw "view-source" missing
- [~] Chromecast sender - `cast discover` / `cast play <name> <url>` work (user/lib/cast + /bin/cast); no HTTP file server for casting local media
- [ ] AirPlay receiver - publish `_airplay._tcp`/`_raop._tcp` (add them to /etc/mdns.conf), RTSP/HTTP server, RTP receive + jitter buffer + timing sync, AirPlay 2 pair-setup/pair-verify (the crypto primitives exist, the HAP wiring does not)
- [ ] IGMP membership + NIC multicast-filter programming - multicast RX works only because the NIC accepts all multicast (fine on SLIRP, a silent-drop risk on real e1000/igb)
- [ ] Compositor: hung-compositor input timeout - only alive/dead is handled, so a hung compositor keeps the keyboard from the recovery console
- Removing "(void)x;" to silence -Wunused-parameter warnings, it's WARNINGS we don't give a fuck.
