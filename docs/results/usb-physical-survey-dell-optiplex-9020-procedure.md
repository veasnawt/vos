# First physical USB survey, Dell OptiPlex 9020: owner procedure

**Nothing here has been run on your Dell yet. Do not start until you tell me you approve the physical boot.**
The survey only reads PCI and USB-controller registers and prints them. It never writes to any disk, installs anything, changes
a BIOS setting or starts a USB driver. It stops on the screen when it is done and needs no key presses after you pick it.

## 1. The files and their checksums

Both are in `/home/veasnawt/src/vos-usb-survey-iso/build/sv-1/` (from Windows, paste into File Explorer: `\\wsl.localhost\Ubuntu\home\veasnawt\src\vos-usb-survey-iso\build\sv-1\`; the files are
`\\wsl.localhost\Ubuntu\home\veasnawt\src\vos-usb-survey-iso\build\sv-1\vos-usb-survey-dual.iso` and
`\\wsl.localhost\Ubuntu\home\veasnawt\src\vos-usb-survey-iso\build\sv-1\vos-usb-survey-bios-only.iso`; copy them to a Windows folder first).
Built from the merged source `6d6a941` (PR #36); the kernel inside is identical to the one verified in Increment 1.

| Use | File | Size | SHA-256 |
| --- | --- | --- | --- |
| **Primary** (boots under Legacy BIOS and UEFI) | `vos-usb-survey-dual.iso` | 13,686,784 B | `736e5fdeb8b6b32c28374d6dee5b5be4bedef18d39cbede05517e69e0a887535` |
| Fallback, only if the primary does not boot on the Dell (Legacy BIOS only) | `vos-usb-survey-bios-only.iso` | 6,752,256 B | `a663ff295ead309c82e2c08dea382bb8e9ee549fd910374b2230ac60e587d982` |

In PowerShell, check the file you copied before writing it:
`Get-FileHash -Algorithm SHA256 .\vos-usb-survey-dual.iso` (the value must match exactly; if it does not, do not use it).

## 2. Write the USB drive (Rufus, DD Image mode)

These images are "ISOHybrid": they carry their own boot sector and partition table, so they must be written byte for byte.
Rufus's other mode (ISO Image mode) rebuilds the drive with its own bootloader files, which is what produced the GRUB errors
you saw before. Use **DD Image mode**.

1. Plug in **only the USB drive you prepared**. The data on it will be erased. Run Rufus.
2. **Device:** pick that USB drive (check the size and name). Leave "List USB hard drives" unticked so the internal disk is never offered.
3. **Boot selection:** click SELECT and choose the `.iso` above (do not use "Disk or ISO image" with a different file).
4. Click START. Rufus says an **ISOHybrid image** was detected and asks how to write it: choose **"Write in DD Image mode"**, then OK.
   Partition scheme and target system can be ignored (the image defines them).
5. When it finishes, Windows may say the drive "needs to be formatted" or look empty. **Do not format it.** Eject it safely and unplug it.

## 3. Boot the Dell from it (nothing on the internal drive is touched)

1. Shut Windows down completely. Plug the USB drive into a rear USB port.
2. Power on and tap **F12** repeatedly until the Dell **one-time boot menu** appears. Do **not** enter BIOS Setup and do not change any setting.
3. Choose the **USB storage device** entry under Legacy boot (it is named after your drive, often "USB Storage Device").
   Do not choose the internal drive or "Windows Boot Manager".
4. A GRUB menu appears and counts down **30 seconds**. Press **Down** once to highlight
   `>>> VOS USB SURVEY: read-only, stops on screen  [build 6d6a941] <<<` and press **Enter**.
   (The first entry, "VOS normal boot", is the default and is harmless: it only prints a greeting. If you do nothing it starts after 30 s.)
   The title lines show the build `6d6a941`, so you can confirm you booted the right image.
5. Text fills the screen and then **stops**. It will stay as it is until you power off. No keyboard or mouse input is needed after step 4.

## 4. Photograph the result

1. Leave the machine alone once the last line reads **`usb: survey complete. Nothing was written. The machine is halted: power it off.`**
2. Take one photo of the whole screen, steady and in focus, with no glare, so that every line is readable. Then take two close-ups
   (top half, bottom half). Make sure the last four lines (`SUMMARY ...`) are in a photo.
3. Send the photos to me. If any line is cut off or unreadable, retake it; the text does not change.

## 5. Shut down safely, and what to do if something goes wrong

- **Normal end:** hold the power button for about 5 seconds until the machine turns off (the survey halts the CPU, so a short press may not
  do anything), then unplug the USB drive. Boot Windows as usual; it is unchanged.
- **Black screen or no text for about 60 seconds after you pressed Enter, or a frozen menu:** hold the power button 5 seconds, unplug the USB drive,
  boot Windows normally. Tell me exactly what you saw last (photo if there is text).
- **No GRUB menu at all, or "no bootable device":** power off, write `vos-usb-survey-bios-only.iso` the same way (DD Image mode) and try again;
  tell me which image failed.
- **Keyboard does not move the GRUB menu:** the default entry will boot after 30 s (harmless); power off and tell me. Do not change BIOS settings to fix it.
- Nothing needs to be repaired afterwards: the survey writes nothing, and there is no installer in this image. Never type anything that looks like
  an install, partition or format command.

## 6. Before the boot: write down (or photograph) these, for me

Which USB ports your keyboard and mouse use (front or rear, blue USB 3.0 or black USB 2.0), and the USB flash drive's port. Also, from Windows, the
**hardware IDs** of every "USB controller" entry in Device Manager (Properties -> Details -> Hardware Ids): the xHCI (`8086:8C31`, which you have)
and any EHCI controllers (your model probably has two). I will compare them with what the survey prints.
