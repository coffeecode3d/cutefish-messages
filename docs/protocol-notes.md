# The KDE Connect side of Messages

Messages talks to Android phones over the published KDE Connect protocol. None
of KDE's code runs here: this is a small implementation of the parts we use,
written from the protocol reference and checked against a Pixel 7 Pro.

## What the phone does, and what we do

1. We broadcast our identity over UDP 1716, and answer the phone's broadcasts.
   The phone is the one that opens the TCP connection, to port 1716.
2. The phone sends its identity **in the clear** as the first thing on the
   connection. We read it, check `targetDeviceId`, and learn its name and id.
3. The **listening side is the TLS client**. `SSL_connect`, with our own
   certificate; the phone is the TLS server. Presenting a client certificate is
   required.
4. Over TLS we send our identity again, then the packets below.

## Packets are newline-delimited JSON

Not length-prefixed. Every packet is one line:

```
{"id": 1712345678901, "type": "kdeconnect.sms.messages", "body": {...}}\n
```

Reading four bytes as a big-endian length instead is the quickest way to spend
an evening debugging an "unexpected eof" from the phone.

## Capabilities gate what the other side will send

A peer refuses to send a packet whose type is not in **our**
`incomingCapabilities`. If the phone is silent, the identity is usually the
reason. Ours:

```
incomingCapabilities:
  kdeconnect.battery
  kdeconnect.sms.messages
  kdeconnect.contacts.response_uids_timestamps
  kdeconnect.contacts.response_vcards
  kdeconnect.pair
outgoingCapabilities:
  kdeconnect.battery.request
  kdeconnect.sms.request
  kdeconnect.sms.request_conversations
  kdeconnect.sms.request_conversation
  kdeconnect.sms.request_attachment
  kdeconnect.contacts.request_all_uids_timestamps
  kdeconnect.contacts.request_vcards_by_uid
  kdeconnect.pair
```

Device ids are 32 hex characters, not dashed UUIDs.

## Pairing

The phone starts pairing from its own screen ("Request pairing"), or by
receiving a `kdeconnect.pair` request from us. The side that receives
`{"pair": true, "timestamp": T}` must answer with the same `pair: true` and the
same `timestamp`, or the phone spins for a while and offers the button again.

The verification code both screens show is the first four bytes, as uppercase
hex, of

```
SHA-256(sorted(our certificate PEM, their certificate PEM) + decimal timestamp)
```

Pairing survives restarts as long as the certificate does; the phone pins it.

## The SMS packets we use

- `kdeconnect.sms.request_conversations`, empty body: the phone answers with
  **one packet per thread**, each containing that thread's latest message.
  Aggregate by `thread_id`; do not treat them as thread replies.
- `kdeconnect.sms.request_conversation`, `{"threadID": N}`: every message in the
  thread, oldest first.
- `kdeconnect.sms.request`, `{"addresses": [{"address": ...}],
  "messageBody": ..., "version": 2}`: send through the phone's SIM.
- `kdeconnect.sms.messages` carries the messages in `body.messages`:
  `_id`, `thread_id`, `addresses[0].address`, `body`, `date` (epoch ms),
  `type` (1 received, 2 sent), `read`, and optional `attachments` with
  `mime_type` and an `encoded_thumbnail` (base64).

## Contacts

- `kdeconnect.contacts.request_all_uids_timestamps` (empty body) →
  `kdeconnect.contacts.response_uids_timestamps` with `uids`.
- `kdeconnect.contacts.request_vcards_by_uid` `{"uids": [...]}` →
  `kdeconnect.contacts.response_vcards`, whose body maps each uid to a vCard
  string. Parse `FN` for the name and every `TEL` for its numbers; compare
  numbers by their digits, with a leading country code dropped.

## Reconnecting

The phone reconnects on its own schedule (app open, network change, or after
its backoff), and a unicast identity to its last address makes it act sooner
than a broadcast. Accept a new connection when the old one is gone; the phone
reuses the same device id and certificate, so no new pairing is needed.
