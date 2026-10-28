# Paperdesk grayscale JPEG decoder

This component vendors TJpgDec R0.03 from Espressif's `esp_jpeg` directory,
at commit `d9d4f733b89389576fe7e5f58ffc321b81468b37` in
<https://github.com/espressif/idf-extra-components>.

`tjpgd.c` is unchanged. Its embedded ChaN copyright and permissive license
notice are retained at the beginning of the file. The upstream component's
Apache-2.0 `LICENSE` is also retained; it does not replace the third-party
notice in TJpgDec itself.

Local adaptations:

- `tjpgdcnf.h` uses a fixed portable configuration: grayscale output,
  1/2/4/8 scaling, a 1 KiB input buffer, and no fast Huffman lookup table.
- Public symbols are prefixed with `paperdesk_` to avoid ROM symbol conflicts.
- `tjpgd.h` uses `<stdint.h>` on all targets, including modern Windows
  compilers. Its other contents are unchanged.

Source URLs, original SHA-256 values and local-file SHA-256 values are recorded
in `source.json`. The C++ wrapper in `main/gallery_image.cpp` limits the input
to 768 KiB and 4096 pixels per dimension, allocates at most 1 MiB of grayscale
image memory, and reports unsupported progressive JPEGs without replacing
the existing gallery image.
