# The Signal backend

Signal has no protocol library a third-party app can link against, and we are
not reimplementing the Signal Protocol. The backend therefore drives
[signal-cli](https://github.com/AsamK/signal-cli), the mature community client,
as a local helper — the same shape as BlueFerry for the iPhone: a separate
process, present only for people who add Signal.

Verified against signal-cli 0.14.10, native build (no JVM):

- `signal-cli` ships a single native 408 MB binary; no Java is needed.
- `signal-cli daemon --socket=<path>` starts multi-account JSON-RPC 2.0 over a
  Unix socket, newline-delimited. `listAccounts` answers `[]` before linking.
- In multi-account mode the daemon exposes `startLink` and `finishLink`; the
  app links itself from the Services sheet without a separate CLI process.
  `startLink` returns the `sgnl://linkdevice?...` URI for the QR code.
- The daemon exposes contacts, groups, sending, receipts, typing and receive
  notifications. It does **not** expose stored conversation history, so the
  backend keeps its own store.

## Shape

```
cutefish-messages ──JSON-RPC/UDS──▶ signal-cli daemon ──▶ Signal service
        │                                    ▲
        └──────── local message store ───────┘  (threads, messages, read state)
```

1. The Services sheet offers **Signal**. Choosing it starts the link flow.
2. `startLink` returns the `sgnl://` URI, which is drawn as a QR code in the
   sheet; `finishLink` waits for the phone to confirm. signal-cli stores the
   account under its own data dir.
3. The backend runs `signal-cli daemon --socket=$XDG_RUNTIME_DIR/cutefish-messages-signal.sock`
   in multi-account mode, so each account-scoped call names its account.
4. Conversations come from the local store, seeded by `listContacts` and
   `listGroups`; incoming envelopes arrive as daemon notifications. Everything
   the user sends is recorded locally when the send succeeds.
5. Typing and read receipts map directly: `sendTyping` and `sendReceipt` out,
   `typingMessage` and `receiptMessage` in. This is the feature set SMS cannot
   offer.

## Store

A small SQLite database under `~/.local/share/cutefish/messages/signal.db`
(system `sqlite3` is the only new dependency), with tables for threads,
messages, participants and the last-seen envelope timestamp. It is what makes
history and the sidebar survive a restart; signal-cli deliberately keeps no
queryable history API.

## Limits to state plainly

- signal-cli is not an official Signal client. It works, but Signal's terms do
  not promise it will keep working.
- The helper is optional: without it, the app is SMS/MMS only.
- The store is plaintext on disk unless the user's home is encrypted.
- Linking requires the phone: Signal shows the QR scanner in *Linked devices*.
- Group support and attachments are part of the same work, not a later phase.

## Status

Implemented: the JSON-RPC client, daemon spawning, linking through
`startLink`/`finishLink`, the QR shown in a sheet (libqrencode), incoming and
outgoing messages, sends, group threads from `listGroups`, contact names from
`listContacts`, read receipts in both directions, typing indicators, profile
pictures from `getAvatar`, local conversation deletion, and a SQLite store
(`signal.db`) for threads, messages, read state and unread marks.

The sidebar shows one service at a time (SMS by default), with an All
conversations view in the Services sheet.

Next: attachments and the service enable/disable state in the config.
