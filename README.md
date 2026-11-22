# Messages

A native, all-in-one messaging app for CutefishOS.

![Messages](docs/screenshot.png)

Messages brings your phone's SMS and MMS and your Signal conversations into one
quiet window built with the Cutefish Toolkit. It follows CutefishOS's design
language — liquid glass, generous radii, a frosted sidebar and a conversation
that reads like paper — and it is a native C++ application with no Qt or KDE
runtime underneath.

## Features

- **One window, every service.** SMS/MMS from your Android phone and Signal,
  shown one service at a time (SMS by default) or together under *All
  conversations*.
- **No KDE runtime.** The app speaks the published KDE Connect protocol itself;
  the only thing installed on the phone is the normal KDE Connect app.
- **Signal as a linked device**, through a native `signal-cli` helper with no
  JVM. Optional, and its history, groups and read state live in a small local
  SQLite store.
- **Conversations that feel finished.** Contact names and pictures, groups,
  channels and forum topics with speakers above each bubble, links that open in
  the browser, timestamps where a conversation breaks, and unread dots.
- **Pictures and files.** Inline pictures, a full-size viewer with **Save
  Photo**, and sending attachments alongside text from the composer.
- **Presence where the service has it.** Signal read receipts show as
  `Sent`/`Read` under your last bubble, and the other side's typing appears
  under the conversation name.
- **Desktop notifications** for messages that arrive when their conversation
  is not on screen; clicking one opens it.
- **Conversation actions.** Right-click or hold a row to mark it read or
  unread, mark every conversation read, or delete local history where the
  service allows it.

## Services

| Service | Status | Backend |
| --- | --- | --- |
| Phone (SMS & MMS) | Ready | Our own KDE Connect protocol implementation |
| Signal | Ready | Native `signal-cli` as a linked device |
| Telegram | Ready | TDLib (`libtdjson`) with your own API id, linked by QR |
| iPhone (BlueFerry) | Later | — |
| RCS (Google Messages) | Not available | RCS is closed to third-party clients |

## Requirements

- CutefishOS with the Cutefish Toolkit (`CutefishToolkit`), C++20 and OpenSSL.
- For SMS: an Android phone on the same network with KDE Connect installed.
- For Signal: the native `signal-cli` binary, pointed at with
  `CUTEFISH_SIGNAL_CLI` when it is not on `PATH`.
- For Telegram: TDLib (`libtdjson`, optional at build time) and your own
  Telegram API id and hash.
- Build tools: CMake and Ninja. `libqrencode` and `sqlite3` are used for the
  Signal linking code and store.

## Install

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
sudo cmake --install build
```

That installs the app, its desktop entry and icon.

## Use

- **Phone:** open KDE Connect on the phone, choose the computer and pair. The
  conversations appear when the phone is on the same network.
- **Signal:** open the footer (it names the active service), choose **Signal**
  and scan the linking code from *Signal → Settings → Linked devices → Link
  new device*. Once linked, Signal joins the service list with history and
  media.
- **Telegram:** create a pair of API credentials at
  <https://my.telegram.org> (API development tools), then put them in
  `~/.config/cutefish/messages/telegram.conf` as `api_id=...` and
  `api_hash=...` (or set `CUTEFISH_TELEGRAM_API_ID` and
  `CUTEFISH_TELEGRAM_API_HASH`). Choose **Telegram** in the footer; the first
  time, paste the API id and hash into the app, which stores them only on this
  computer. Then scan the code from *Telegram → Settings → Devices → Link
  Desktop Device*; the app asks for your two-step password if the account has
  one, and falls back to a phone number and code if the QR request fails.
- Switch services from the footer at any time. **All conversations** mixes
  them, with each preview labelled by its service.
- Attach a picture or file with the **+** beside the composer. Click a picture
  to view it and use **Save** to keep a copy.

## Development

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
build/cutefish-messages
```

`--demo` opens the window with made-up conversations for working on the
interface without a phone.

`docs/protocol-notes.md` records the KDE Connect packet details the SMS
backend relies on. `docs/signal-backend.md` describes the Signal helper, its
store and its limits.

## Limits

- RCS is not available to third-party clients; RCS chats stay in the phone's
  own app.
- Sending MMS needs the phone's messaging app to be the default SMS app; plain
  SMS always sends.
- Signal is reached through the community `signal-cli`; Signal does not promise
  that unofficial clients keep working.
- iPhone support is waiting on BlueFerry and hardware to test it against.
- Telegram requires your own API id and hash, because Telegram does not issue
  one to third-party clients automatically.
- Unread dots on the phone side are kept locally; the KDE Connect protocol has
  no packet to mark a thread read on the phone.

## Licence

BSD-3-Clause. See `LICENSE`.
