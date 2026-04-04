/* slopay-loader-test.c
 *
 * Fixture-based tests for the AY file loader.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "slopay-loader.h"

static void expect_true(int condition, const char *expression, const char *file, int line)
{
  if (!condition) {
    fprintf(stderr, "Expectation failed: %s (%s:%d)\n", expression, file, line);
    abort();
  }
}

#define EXPECT(expr) expect_true((expr), #expr, __FILE__, __LINE__)

static int write_fixture_file(const char *path)
{
  static const uint8_t fixture[] = {
    'Z', 'X', 'A', 'Y',
    'E', 'M', 'U', 'L',
    0x01, 0x03,
    0x00, 0x00,
    0x00, 0x0C,
    0x00, 0x11,
    0x00,
    0x00,
    0x00, 0x02,
    0x00, 0x13,
    0x00, 0x1B,
    'T', 'e', 's', 't', 'e', 'r', 0x00,
    'F', 'i', 'x', 't', 'u', 'r', 'e', 0x00,
    'U', 'n', 'i', 't', ' ', 'T', 'u', 'n', 'e', 0x00,
    0x01, 0x02, 0x03, 0x04,
    0x00, 0x64,
    0x00, 0x14,
    0x12, 0x34,
    0x00, 0x04,
    0x00, 0x08,
    0x80, 0x00,
    0x12, 0x34,
    0x56, 0x78,
    0x40, 0x00,
    0x00, 0x04,
    0x00, 0x08,
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00,
    0xDE, 0xAD, 0xBE, 0xEF
  };
  FILE *f = fopen(path, "wb");
  size_t written;
  int close_result;

  if (f == NULL)
    return -1;

  written = fwrite(fixture, 1, sizeof(fixture), f);
  close_result = fclose(f);
  if (written != sizeof(fixture) || close_result != 0)
    return -1;

  return 0;
}

static void test_loader_fixture(void)
{
  char path[] = "/tmp/slopay-loader-test-XXXXXX";
  int fd = mkstemp(path);
  int close_result;
  slopay_loader_file_t *file;
  slopay_loader_song_t *song;
  char *song_name;

  EXPECT(fd >= 0);
  close_result = close(fd);
  EXPECT(close_result == 0);

  EXPECT(write_fixture_file(path) == 0);

  file = slopay_loader_load_file(path);
  EXPECT(file != NULL);
  EXPECT(file->header.file_version == 1);
  EXPECT(file->header.player_version == 3);
  EXPECT(slopay_loader_get_num_songs(file) == 1);
  EXPECT(strcmp(slopay_loader_get_author(file), "Tester") == 0);
  EXPECT(strcmp(slopay_loader_get_misc_info(file), "Fixture") == 0);

  song_name = slopay_loader_get_song_name(file, 0);
  EXPECT(song_name != NULL);
  EXPECT(strcmp(song_name, "Unit Tune") == 0);

  song = slopay_loader_load_song(file, 0);
  EXPECT(song != NULL);
  EXPECT(strcmp(song->name, "Unit Tune") == 0);
  EXPECT(song->song_data.a_chan == 1);
  EXPECT(song->song_data.b_chan == 2);
  EXPECT(song->song_data.c_chan == 3);
  EXPECT(song->song_data.noise_chan == 4);
  EXPECT(song->song_data.song_length == 100);
  EXPECT(song->song_data.fade_length == 20);
  EXPECT(song->song_data.hi_reg == 0x12);
  EXPECT(song->song_data.lo_reg == 0x34);
  EXPECT(song->pointers.stack == 0x8000);
  EXPECT(song->pointers.init == 0x1234);
  EXPECT(song->pointers.interrupt == 0x5678);
  EXPECT(song->blocks != NULL);
  EXPECT(song->blocks[0].address == 0x4000);
  EXPECT(song->blocks[0].length == 4);
  EXPECT(song->blocks[0].offset == 8);
  EXPECT(song->blocks[1].address == 0);

  free(song_name);
  slopay_loader_song_destroy(song);
  slopay_loader_file_destroy(file);
  EXPECT(unlink(path) == 0);
}

int main(void)
{
  test_loader_fixture();
  return 0;
}
