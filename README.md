# Klyraudio

DOWNLOAD from releases on the right ----------------------------------------------------------------------------------------------------------------------------------------------->

A small Winamp-style audio player for Windows. Klyraudio plays your local music, web radio, and podcasts, and you can arrange its windows any way you like.

Version 1.6.6.

## Requirements

- Windows 10 (version 1809) or later, or Windows 11
- OGG Vorbis and Opus files need the free "Web Media Extensions" from the Microsoft Store. Everything else works without it.

Klyraudio doesn't need an installer. Unzip the folder anywhere and run `Klyraudio.exe`.

## Getting started

1. Run `Klyraudio.exe`.
2. Add music in any of these ways:
   - Drag files or folders onto the window or the playlist.
   - Use **File > Add Files** (Ctrl+O) or **File > Add Folder** (Ctrl+D).
   - Pass files on the command line: `Klyraudio.exe "C:\path\to\song.mp3"`.
3. Double-click a track, or select it and press Enter, to play it.

Klyraudio remembers your playlist, volume, and window layout, and restores them the next time you start it.

## The windows

Klyraudio has four windows:

- **Main window**: the display, the seek bar, the transport buttons, and the volume.
- **Playlist**: your tracks. Open or close it with **View > Playlist window** (Alt+2).
- **Equalizer**: 10 bands and a preamp. Open it with the EQ button or Ctrl+E.
- **Podcasts**: your podcast subscriptions and episodes. Open it with Ctrl+P.

### Snapping

The windows snap to each other. Drag a window near another one and its edges pull into line. Snapped windows move together.

- To move a window and leave the others, hold **Shift** when you start dragging it.
- Windows also snap to the edges of the screen.
- The main window has a fixed height. You can make it wider, but not taller.

The playlist window hides and minimises with the main window, and it has no taskbar button of its own.

## Playlist

- Double-click a track, or press Enter, to play it.
- Right-click the playlist for more options: play, copy the title or file path, show the file in Explorer, file properties, sort, select all, and remove.
- Press Delete to remove the selected tracks.
- **File > Save Playlist** saves a `.klyr` playlist. **File > Open Playlist** loads one.
- Klyraudio also opens `.m3u`, `.m3u8`, `.small`, and `.cumdrops` playlists.

## Playback

- **Gapless playback** (Playback menu) starts the next track without a pause. It's on by default.
- **Crossfade** (Playback > Crossfade) fades one track into the next. Choose Off, 1, 2, 3, 5, 8, or 12 seconds. It's off by default.
- Shuffle and repeat are on the transport row. Repeat cycles through off, all, and one.
- Gapless and crossfade work for automatic track changes. Next and Previous start the track straight away.

## Web radio and podcasts

- **File > Add URL** (Ctrl+U) adds a web radio stream, a `.pls` or `.m3u` station list, or a podcast RSS feed.
- Live radio can't be seeked, because it has no length.
- Podcasts add one entry per episode. Klyraudio remembers which episodes you've played.
- In the Podcasts window you can subscribe by feed address, search Apple's podcast directory by name, import and export OPML files, and copy episode links.

## Equalizer

Open the equalizer with the EQ button or Ctrl+E. Use **Enable EQ** to turn it on or off, and **Reset** to flatten it. The preamp and the ten band sliders go from -12 to +12 dB.

## Keyboard shortcuts

| Action | Default key |
|---|---|
| Play | X |
| Pause / resume | C |
| Stop | V |
| Previous | Z |
| Next | B |
| Shuffle | S |
| Repeat | R |
| Back 5 seconds | Ctrl+Left |
| Forward 5 seconds | Ctrl+Right |
| Volume up | Ctrl+Up |
| Volume down | Ctrl+Down |
| Add files | Ctrl+O |
| Add folder | Ctrl+D |
| Add URL | Ctrl+U |
| Open playlist | Ctrl+L |
| Save playlist | Ctrl+S |
| Select all | Ctrl+A |
| Remove selected | Delete |
| Play selected | Enter |
| Equalizer | Ctrl+E |
| Podcasts | Ctrl+P |
| Playlist window | Alt+2 |

### Changing shortcuts

Open **View > Keyboard shortcuts**. Select an action, then:

- **Window key...** sets the shortcut that works while Klyraudio is focused.
- **Global hotkey...** sets a shortcut that works from any program. Global hotkeys can't use the Windows key, and some keys can't be used for them.
- **Clear** removes both shortcuts for the selected action.
- **Reset all** restores the defaults.

If you pick a key that another action already uses, Klyraudio asks before moving it. A global hotkey that another program already uses shows "(in use)" in the list.

## Your files and settings

Klyraudio keeps everything in a `data` folder next to `Klyraudio.exe`. That makes the program portable: copy the whole folder to another PC and your settings come with it.

- `settings.ini`: volume, window layout, EQ, and shortcuts.
- `last.klyr`: your last playlist.
- `podcasts.dat`: your podcast subscriptions and played episodes.

If the folder can't be written to, Klyraudio uses `%APPDATA%\Klyraudio` instead.

Shortcuts are stored in `settings.ini` as `key_<action>` (window shortcut) and `gkey_<action>` (global hotkey). Only changed shortcuts are written.

## Known limits

- Gapless playback can't remove the encoder padding inside MP3 and AAC files. How much silence you hear depends on the Windows decoder.
- If two neighbouring tracks have different channel layouts, Klyraudio falls back to a normal gap between them.
- The shortcut dialog can't record the Windows key or media keys. You can still set them by editing `settings.ini`, but only global hotkeys will use them.
- Snapping only applies to the four windows above. Resizing one window doesn't move the windows snapped to it.

