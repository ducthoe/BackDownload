# BackDownload

by ducttape3

Unlocks Download Mode on Samsung devices with a screen lock enabled. Sets the
DMC AT authorization flag after boot. Requires Root and Zygisk.

Install the ZIP through your root manager and reboot into Android.
Tap **Action** to read the current policy flags and Download Mode status.

## Build on Linux

Install Python 3 and the [Android NDK](https://developer.android.com/ndk/guides/other_build_systems), then run from this folder:

```sh
python3 build.py --ndk /path/to/android-ndk
```

The installable ZIP is in `dist/`.

GPLv3. See [LICENSE](LICENSE).
