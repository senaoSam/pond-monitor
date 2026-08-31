// Measures, rather than guesses, why an OTA image that was written correctly
// still fails validation.
//
// The stuck fact on the watchdog board: after a failed activation, dumping the
// target slot over USB and comparing against the release matched byte for
// byte -- yet esp_ota_set_boot_partition() kept returning
// ESP_ERR_OTA_VALIDATE_FAILED with "Image hash failed - image is corrupt".
// Correct data failing verification means the two sides read the flash
// differently, and there really are two read paths:
//
//   raw   esp_partition_read()  -> SPI transaction, no cache involved.
//         This is the path an esptool dump resembles.
//   mmap  esp_partition_mmap()  -> through the MMU and the flash cache.
//         This is the path esp_image_verify() actually uses.
//
// This probe hashes the image over both paths and diffs them chunk by chunk.
// If raw == downloaded bytes but mmap disagrees, the data is fine and the
// cache is serving stale lines -- and the diff says exactly which offsets.
// If raw itself is wrong, the write path is at fault. Either way the next
// fwlog entry names the mechanism instead of inviting an eighth theory.
//
// Kept separate from pull_ota.h so it can be called on demand (an HTTP
// /verify endpoint) as well as from the failure path.

#pragma once

#include <Arduino.h>
#include <string.h>
#include <esp_err.h>
#include <esp_image_format.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>
#include <esp_task_wdt.h>
#include "mbedtls/sha256.h"

struct FlashProbeResult {
  bool ran = false;        // false = could not even start (see note)
  size_t imageLen = 0;     // full image incl. checksum padding and SHA suffix
  bool lenFromHeader = false;  // length derived by walking the image itself
  String shaRaw;           // esp_partition_read path
  String shaMmap;          // mmap/cache path ("" if mmap failed)
  int diffChunks = -1;     // 4KB chunks where the two paths disagree
  size_t firstDiff = 0;    // absolute offset of the first differing byte
  String firstDiffHex;     // raw vs mmap bytes at that offset
  esp_err_t imgVerify = ESP_FAIL;  // esp_image_verify(), run LAST (it mmaps)
  String note;
};

static String probeHex(const uint8_t *b, size_t n) {
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    char h[3];
    snprintf(h, sizeof(h), "%02x", b[i]);
    s += h;
  }
  return s;
}

// Derives the total image length by walking the header and segment table via
// raw reads, so the probe needs no outside knowledge of the .bin size.
// Layout: header, N segments, one checksum byte padded to a 16-byte boundary,
// then a 32-byte SHA256 when hash_appended is set.
static bool probeImageLen(const esp_partition_t *p, size_t *outLen) {
  esp_image_header_t hdr = {};
  if (esp_partition_read(p, 0, &hdr, sizeof(hdr)) != ESP_OK) return false;
  if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) return false;
  if (hdr.segment_count == 0 || hdr.segment_count > ESP_IMAGE_MAX_SEGMENTS)
    return false;

  size_t off = sizeof(esp_image_header_t);
  for (int i = 0; i < hdr.segment_count; i++) {
    esp_image_segment_header_t seg = {};
    if (esp_partition_read(p, off, &seg, sizeof(seg)) != ESP_OK) return false;
    if (seg.data_len >= p->size) return false;  // walked into garbage
    off += sizeof(seg) + seg.data_len;
    if (off >= p->size) return false;
  }
  off = (off + 1 + 15) & ~(size_t)15;  // checksum byte, padded
  if (hdr.hash_appended == 1) off += 32;
  if (off >= p->size) return false;
  *outLen = off;
  return true;
}

// knownLen: fallback image length (e.g. the byte count just downloaded) for
// when the header walk fails; pass 0 if unknown.
static FlashProbeResult flashProbe(const esp_partition_t *part,
                                   size_t knownLen) {
  FlashProbeResult r;
  if (!part) {
    r.note = "no partition";
    return r;
  }

  r.lenFromHeader = probeImageLen(part, &r.imageLen);
  if (!r.lenFromHeader) r.imageLen = knownLen;
  if (r.imageLen == 0 || r.imageLen > part->size) {
    r.note = "no usable image length (header walk failed, no fallback)";
    return r;
  }

  uint8_t *buf = (uint8_t *)malloc(4096);
  if (!buf) {
    r.note = "no heap for probe buffer";
    return r;
  }

  // Pass 1: SHA256 via raw SPI reads.
  {
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts_ret(&c, 0);
    for (size_t off = 0; off < r.imageLen; off += 4096) {
      esp_task_wdt_reset();
      size_t n = min((size_t)4096, r.imageLen - off);
      if (esp_partition_read(part, off, buf, n) != ESP_OK) {
        r.note = "raw read failed at 0x" + String(off, HEX);
        mbedtls_sha256_free(&c);
        free(buf);
        return r;
      }
      mbedtls_sha256_update_ret(&c, buf, n);
    }
    uint8_t d[32];
    mbedtls_sha256_finish_ret(&c, d);
    mbedtls_sha256_free(&c);
    r.shaRaw = probeHex(d, 32);
  }

  // Pass 2: SHA256 via the cache path, and a chunk diff against raw. This is
  // the same window esp_image_verify() looks through.
  const void *mem = nullptr;
  spi_flash_mmap_handle_t mh = 0;
  esp_err_t me =
      esp_partition_mmap(part, 0, r.imageLen, SPI_FLASH_MMAP_DATA, &mem, &mh);
  if (me != ESP_OK) {
    r.note = String("mmap failed: ") + esp_err_to_name(me);
  } else {
    const uint8_t *m = (const uint8_t *)mem;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts_ret(&c, 0);
    r.diffChunks = 0;
    bool haveFirst = false;
    for (size_t off = 0; off < r.imageLen; off += 4096) {
      esp_task_wdt_reset();
      size_t n = min((size_t)4096, r.imageLen - off);
      mbedtls_sha256_update_ret(&c, m + off, n);

      if (esp_partition_read(part, off, buf, n) == ESP_OK &&
          memcmp(buf, m + off, n) != 0) {
        r.diffChunks++;
        if (!haveFirst) {
          haveFirst = true;
          size_t i = 0;
          while (i < n && buf[i] == m[off + i]) i++;
          r.firstDiff = off + i;
          size_t left = min(i, (size_t)8);
          size_t span = min((size_t)16, n - (i - left));
          r.firstDiffHex = "raw=" + probeHex(buf + i - left, span) +
                           " mmap=" + probeHex(m + off + i - left, span) +
                           " (from 0x" + String(r.firstDiff - left, HEX) + ")";
        }
      }
    }
    uint8_t d[32];
    mbedtls_sha256_finish_ret(&c, d);
    mbedtls_sha256_free(&c);
    r.shaMmap = probeHex(d, 32);
    spi_flash_munmap(mh);
  }
  free(buf);

  // Last, because it creates mappings of its own and logs to the console;
  // running it earlier would disturb the cache state being measured.
  {
    esp_partition_pos_t pos;
    pos.offset = part->address;
    pos.size = part->size;
    esp_image_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    r.imgVerify = esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta);
  }

  r.ran = true;
  return r;
}

// One line per fact; served over HTTP and readable in a terminal.
static String flashProbeReport(const FlashProbeResult &r,
                               const String &shaDownloaded) {
  String s;
  s += "probe ran: " + String(r.ran ? "yes" : "NO") + "\n";
  if (r.note.length()) s += "note: " + r.note + "\n";
  s += "image len: " + String(r.imageLen) +
       (r.lenFromHeader ? " (from header walk)\n" : " (fallback)\n");
  if (shaDownloaded.length()) s += "sha256 downloaded: " + shaDownloaded + "\n";
  s += "sha256 raw read:   " + r.shaRaw + "\n";
  s += "sha256 mmap read:  " + r.shaMmap + "\n";
  if (r.diffChunks >= 0) {
    s += "raw-vs-mmap differing 4KB chunks: " + String(r.diffChunks) + "\n";
    if (r.diffChunks > 0) {
      s += "first difference at: 0x" + String(r.firstDiff, HEX) + "\n";
      s += "bytes: " + r.firstDiffHex + "\n";
    }
  }
  s += "esp_image_verify: " + String(esp_err_to_name(r.imgVerify)) + "\n";
  return s;
}
