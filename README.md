# RAM data remanence tester

This UEFI application was made to help with testing RAM data remanence after
powering off the platform.

## Building

Prerequisites:

- `gcc`, `make` & friends
- `gnu-efi`
- `gnu-efi-devel`

It is known to work with GCC 14.2.1 and both `gnu-efi` packages in version
3.0.18, as found in Fedora 40. Older versions of `gnu-efi` may require different
build options than those defined in Makefile.

To build, simply run:

```shell
make
```

This will produce `BOOTx64.EFI` file that should be copied to USB drive
formatted as FAT32 (not exFAT) to `/EFI/BOOT/` directory.

## Use

Plug in the drive and boot the tested platform from it. It will show available
RAM and EFI descriptors. Then, it will start dumping values of RAM addresses
from EFI descriptors into the drive from which it has been booted. The RAM will
be dumped into files with name prepended with the date and time and appended
with the dumping start address. The application will dump RAM only from
addresses specified on the descriptors printed.

## Credits

This research has been supported by Power Up Privacy, a privacy advocacy group
that seeks to supercharge privacy projects with resources so they can complete
their mission of making our world a better place.
