# Testing and commissioning

Test with the **motor disconnected** first: listen for the relay click, or wire a lamp to the relay contacts.

## 1. Bench test (both boards on the table)

- [ ] The relay stays OFF when the receiver powers up and when its RESET button is pressed.
- [ ] The transmitter shows `LINK OK` with a double beep within a few seconds.
- [ ] Hold 1–4 s → motor ON (single beep, green LED). Hold again → OFF.
- [ ] 3 quick taps → `PRESET10`; hold 4 s → `PRESET15`. Each switches OFF on time.
- [ ] Manual ON, then switch the transmitter off → relay OFF within about 10 s (60 s with the SF12 firmware).
- [ ] Start a preset, wait for the beep, switch the transmitter off → the motor keeps running and stops on time.
- [ ] Power-cycle the receiver → the transmitter shows `NO LINK`, then reconnects by itself.

## 2. Field test (final positions)

Connect the transmitter to a laptop, open the Serial Monitor at 115200 baud and let it run for 15–30 minutes. Every ACK line shows the signal quality:

```text
[RX] ACK:41207:OFF:OFF:0 (RSSI -108, SNR -6.5)
```

| Lowest SNR seen | Action |
|---|---|
| −5 dB or better, no `[LINK LOST]` | Keep the default (SF10) firmware |
| Below −5 dB, or `[LINK LOST]` appears | Improve antenna placement, or flash **both** boards from `firmware/sf12-long-range` |

LoRa can decode down to about −15 dB SNR at SF10 and −20 dB at SF12. Keeping 10 dB of margin covers rain, people and doors.

## 3. Long-run test (24–48 hours)

- [ ] Transmitter: few or no `[LINK LOST]` lines.
- [ ] Receiver: `minHeap` in the `[HEALTH]` line stays stable.
- [ ] No unexpected restarts (the `ready` banner appears only once per board).

## Log reference

### Transmitter

| Tag | Meaning |
|---|---|
| `[TX]` | Packet sent |
| `[RX] … (RSSI, SNR)` | Packet received, with signal quality |
| `[LINK OK]` / `[LINK LOST]` | Link restored / no ACK for too long |
| `[COMMAND]`, `[RETRY]`, `[COMMAND CONFIRMED]`, `[COMMAND FAILED]` | Command lifecycle |
| `[COMMAND REJECTED] Link down` | Touch command ignored because there is no link |
| `[MOTOR CONFIRMED]` | Receiver reported a motor state change |
| `[NO ACK]` | (SF12 firmware) an exchange got no reply and will be retried |
| `[TOUCH]` | Touch gestures |
| `[HEALTH]` | Every 5 s: link, ACK age, motor, pending command, free heap |
| `[RADIO]` | Radio re-initialised after a long silence |

### Receiver

| Message | Meaning |
|---|---|
| `Received: … (RSSI, SNR)` | Packet received |
| `ACK sent: …` | Reply sent |
| `Motor: ON/OFF \| Mode: …` | Relay state changed |
| `Duplicate command ID … (retry)` | Retry acknowledged, not executed again |
| `Preset expired: motor OFF` | 10/15-minute run finished |
| `WATCHDOG: transmitter lost during MANUAL run; motor OFF` | Manual run stopped by the link failsafe |
| `[HEALTH]` | Every 10 s: motor state and free heap |
| `RADIO: silent too long; re-initialising LoRa` | Normal while the transmitter is switched off |
