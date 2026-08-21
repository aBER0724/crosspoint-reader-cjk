# CJK Font Support

CrossPoint supports Chinese, Japanese, and Korean glyphs through SD-card
`.cpfont` families. The same family can be assigned to the reader, the user
interface, or both from **Settings > Reader > Text Settings**.

The complete installation, conversion, directory-layout, and UI fallback guide
is maintained in [SD Card Fonts](./sd-card-fonts.md). That document is the
source of truth for the current format and workflow.

## CJK Requirements

- Use the `cjk` conversion preset for Chinese ideographs, Japanese kana, and
  full-width punctuation.
- Add the `hangul` preset when Korean coverage is required.
- Generate 8, 10, and 12 pt files for size-matched UI rendering.
- Generate the standard reader sizes: 14, 16, 18, and 22 pt. Already-installed
  12/14/16/18 families remain compatible.
- Keep every size in one family directory using
  `<Family>/<Family>_<size>.cpfont`.

For example:

```text
/.fonts/NotoSansSC/
  NotoSansSC_8.cpfont
  NotoSansSC_10.cpfont
  NotoSansSC_12.cpfont
  NotoSansSC_14.cpfont
  NotoSansSC_16.cpfont
  NotoSansSC_18.cpfont
  NotoSansSC_22.cpfont
```

## Install And Select

Install a family through **Settings > System > Manage Fonts**, upload `.cpfont`
files from the web interface, or copy the family directory to `/.fonts/` or
`/fonts/` on the SD card. Then assign its role in **Text Settings**.

Fixed-size CJK files are not scaled by the device. Text Settings displays the
actual point size supplied by the selected family; only sizes that exist on the
SD card can be used.

## Custom Conversion

```bash
python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
  MyCJKFont-Regular.otf \
  --intervals latin-ext,cjk \
  --sizes 8,10,12,14,16,18,22 \
  --style regular \
  --name MyCJKFont \
  --output-dir ./MyCJKFont/
```

See [SD Card Fonts](./sd-card-fonts.md#converting-custom-fonts) for multi-style
conversion, interval presets, and troubleshooting.
