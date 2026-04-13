# Artemis — Goddess of the Burn 🏹🔥

**Artemis** is a high-performance, lightweight ISO burning utility for Linux, designed to handle modern hybrid ISOs and legacy BIOS/Windows XP images with ease.

## Features

- **Honest Progress:** Periodic syncing ensures the progress bar reflects real hardware speed, preventing long "Syncing" hangs at the end.
- **Hybrid Fix:** Automatically synthesizes a bootable MBR for legacy/XP images (like Windows XP, TempleOS, or old Linux distros) that aren't natively hybrid.
- **Safety First:** Detects mounted devices, performs partition matching, and requires explicit `yes` confirmation.
- **Verification Mode:** Bit-for-bit verification of the burned image.
- **Device Discovery:** Built-in tool to list removable USB/SD devices.

## Installation

```bash
git clone https://github.com/solace-jpg/artemis.git
cd artemis
make
sudo make install
```

## Usage

### List available devices
```bash
sudo artemis -l
```

### Burn an ISO (with verification)
```bash
sudo artemis -v image.iso /dev/sdX
```

### Skip Hybridization
If you are burning a raw disk image and don't want Artemis to touch the MBR:
```bash
sudo artemis -n image.iso /dev/sdX
```

## Troubleshooting

### "Target device is mounted"
Artemis will refuse to burn to a device that is currently in use. Unmount it first:
```bash
sudo umount /dev/sdX*
```

### Root Privileges
Artemis requires direct access to block devices, so it must be run with `sudo`.

## License
MIT License - see [LICENSE](LICENSE) for details.

---
*The Archer of Images* 🌙
