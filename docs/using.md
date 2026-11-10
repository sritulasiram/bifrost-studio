# Using Bifrost Studio

How to flash a phone, and what to do when something goes wrong. Install and first-run steps are in the [README](../README.md#install). Flashing can leave a phone unbootable or erase its data, so use it only on a phone you can recover, with firmware for that exact model.

## Before you start

- **Firmware.** The `.tar.md5` files for your exact model and region: BL and AP are required for a normal flash; CP and CSC are optional. Bifrost Studio does not download firmware.
- **Services registered.** Settings (⌘,) › Services shows both background services as enabled. See [services.md](services.md) for what they are.
- **A good cable**, plugged directly into the Mac, and other USB tools closed (Smart Switch, Odin in a VM, browsers using WebUSB).

## Enter Download Mode

The key combination depends on the model; this is general Samsung guidance, not something Bifrost Studio verifies:

- Power the phone off.
- Phones with a Home button: hold **Volume Down + Home + Power**.
- Phones with a Bixby button: hold **Volume Down + Bixby + Power**.
- Newer phones with neither: hold **Volume Up + Volume Down** and plug in the USB cable.
- Press **Volume Up** at the warning screen to continue.

If the phone is booted into Android with USB debugging on, Tools › Android › **Reboot to Download Mode** does this for you (unreleased; not yet verified on hardware).

The phone should show a Download Mode screen, and Bifrost Studio's sidebar should list it. If it only says "not in Download Mode", the phone may use a product ID not yet listed (fail-safe; see [status.md](status.md#known-issues)).

## Flash

1. **Setup.** Choose the phone and drop the firmware files in. They are verified on the Mac; the phone is not touched yet.
2. **Review.** Check which image goes to which partition. Anything the planner can't match to the phone is refused here, and nothing is flashed.
3. **Confirm.** Acknowledge any data erase (a full CSC or USERDATA does this) and choose whether to reboot at the end.
4. **Flash.** Keep the phone connected until it finishes. A flash keeps running if you close the window.

**A flash must be the first session after the phone enters Download Mode.** After a probe or PIT backup, restart the phone into Download Mode before flashing. The app blocks Flash until you do (tap "I Restarted It into Download Mode" if it guessed wrong). The reason is in [hardware.md](hardware.md#why-a-flash-must-be-the-first-session).

## When it fails

Every failure reports whether the phone received no data, some data, or all of it. Follow the row that matches.

| What the app says | What it means | What to do |
|---|---|---|
| **Stopped before writing** | No partition data was sent; the phone's software is unchanged. | Fix the cause below, restart into Download Mode and try again. |
| **The phone may be partially written** | Some data was sent. The phone may not start normally. | Don't unplug or restart into Android. Stay in Download Mode (re-enter it if it left) and flash the complete firmware again. If Bifrost Studio can't reconnect, Odin on Windows or Heimdall can also recover a phone in Download Mode. |
| **Data written; closing failed** | Everything was acknowledged; only closing the session or the reboot request failed. | Restart the phone manually (hold Power and Volume Down until it restarts). |

Causes for "Stopped before writing", by error:

| Error | What to do |
|---|---|
| `usb-timeout` | The phone stayed silent. Restart it into Download Mode before retrying: each attempt resets its USB, so repeating in the same Download Mode entry makes it worse, and the app keeps Flash disabled until you restart. |
| `model-mismatch` | Compare the model on the phone's Download Mode screen with the firmware's model. |
| `plan-refused` | The firmware doesn't match the phone's partition table. Read the refusals on the Review step. |
| `service-unavailable` | Register the services in Settings › Services and approve them in Login Items. |
| `usb-access` | Quit other USB tools, then retry. |
| `checksum-mismatch`, `file-changed` | The firmware file is damaged or changed; get it again. |
| anything else | Restart into Download Mode and retry. |

Still stuck: open an issue with the phone model, firmware build and the History log (History › Export › Copy for Bug Report). Error codes are listed in [architecture.md](architecture.md#errors) and [c-api.md](c-api.md#errors).
