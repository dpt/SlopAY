/* slopay-loader.c
 *
 * AY format music file loader (Motorola/big-endian format)
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "slopay-loader.h"

/* Helper: Read 16-bit big-endian value */
static uint16_t read_be16(const uint8_t *data)
{
  return (data[0] << 8) | data[1];
}

/* Helper: Read signed 16-bit big-endian value */
static int16_t read_be16_signed(const uint8_t *data)
{
  return (data[0] << 8) | data[1];
}

static size_t rel_ptr(size_t base, int16_t off)
{
  return (size_t)((int32_t)base + (int32_t)off);
}

/* Helper: Read null-terminated string from file data with relative offset */
static char *slopay_loader_read_string(const uint8_t *file_data, size_t file_size,
                         int16_t offset, size_t pointer_offset, size_t max_len)
{
  size_t pos;
  char *str;
  size_t len = 0;

  if (offset == 0)
    return NULL;

  pos = rel_ptr(pointer_offset, offset);
  if (pos >= file_size)
    return NULL;

  /* Find string length */
  while (pos + len < file_size && file_data[pos + len] != '\0' && len < max_len)
    len++;

  str = malloc(len + 1);
  if (str == NULL)
    return NULL;

  memcpy(str, &file_data[pos], len);
  str[len] = '\0';

  return str;
}

slopay_loader_file_t *slopay_loader_load_file(const char *filename)
{
  FILE *f;
  slopay_loader_file_t *file;
  size_t bytes_read;

  f = fopen(filename, "rb");
  if (f == NULL) {
    fprintf(stderr, "Error: Cannot open AY file: %s\n", filename);
    return NULL;
  }

  file = malloc(sizeof(*file));
  if (file == NULL) {
    fprintf(stderr, "Error: Memory allocation failed\n");
    fclose(f);
    return NULL;
  }

  memset(file, 0, sizeof(*file));

  /* Get file size */
  fseek(f, 0, SEEK_END);
  file->file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  /* Read entire file */
  file->file_data = malloc(file->file_size);
  if (file->file_data == NULL) {
    fprintf(stderr, "Error: Cannot allocate file buffer\n");
    free(file);
    fclose(f);
    return NULL;
  }

  bytes_read = fread(file->file_data, 1, file->file_size, f);
  fclose(f);

  if (bytes_read != file->file_size) {
    fprintf(stderr, "Error: Failed to read complete file\n");
    free(file->file_data);
    free(file);
    return NULL;
  }

  /* Parse header */
  if (file->file_size < 20) {
    fprintf(stderr, "Error: File too small for AY header\n");
    free(file->file_data);
    free(file);
    return NULL;
  }

  memcpy(file->header.file_id, &file->file_data[0], 4);
  memcpy(file->header.type_id, &file->file_data[4], 4);
  file->header.file_version = file->file_data[8];
  file->header.player_version = file->file_data[9];
  file->header.p_special_player = read_be16_signed(&file->file_data[10]);
  file->header.p_author = read_be16_signed(&file->file_data[12]);
  file->header.p_misc = read_be16_signed(&file->file_data[14]);
  file->header.num_of_songs = file->file_data[16];
  file->header.first_song = file->file_data[17];
  file->header.p_songs_structure = read_be16_signed(&file->file_data[18]);

  /* Verify magic */
  if (memcmp(file->header.file_id, "ZXAY", 4) != 0) {
    fprintf(stderr, "Error: Invalid magic (expected 'ZXAY')\n");
    free(file->file_data);
    free(file);
    return NULL;
  }

  printf("AY File: Type %c%c%c%c, Version %d, Player %d\n",
         file->header.type_id[0], file->header.type_id[1],
         file->header.type_id[2], file->header.type_id[3],
         file->header.file_version, file->header.player_version);

  /* Read author and misc info */
  file->author = slopay_loader_read_string(file->file_data, file->file_size,
                              file->header.p_author, 12, 256);
  file->misc_info = slopay_loader_read_string(file->file_data, file->file_size,
                                 file->header.p_misc, 14, 512);

  if (file->author)
    printf("Author: %s\n", file->author);

  /* Number of songs */
  file->num_songs = file->header.num_of_songs + 1;
  printf("Number of songs: %d\n", file->num_songs);

  /* Read song structures */
  file->songs = malloc(file->num_songs * sizeof(slopay_loader_song_structure_t));
  if (file->songs == NULL) {
    fprintf(stderr, "Error: Cannot allocate song structures\n");
    free(file->author);
    free(file->misc_info);
    free(file->file_data);
    free(file);
    return NULL;
  }

  /* Calculate actual song structure offset */
  /* All offsets are relative to their position in the file */
  int32_t base_offset = 18 + (int32_t)file->header.p_songs_structure;
  size_t songs_base_offset = (size_t)base_offset;
  size_t songs_offset = songs_base_offset;

  for (int i = 0; i < file->num_songs; i++) {
    if (songs_offset + 4 > file->file_size) {
      fprintf(stderr, "Error: Invalid song structure offset\n");
      free(file->songs);
      free(file->author);
      free(file->misc_info);
      free(file->file_data);
      free(file);
      return NULL;
    }

    file->songs[i].p_song_name = read_be16_signed(&file->file_data[songs_offset]);
    file->songs[i].p_song_data = read_be16_signed(&file->file_data[songs_offset + 2]);

    /* Store the offset of this song structure for later use */
    file->songs[i]._struct_offset = songs_offset;

    songs_offset += 4;
  }

  return file;
}

void slopay_loader_file_destroy(slopay_loader_file_t *file)
{
  if (file == NULL)
    return;

  if (file->songs)
    free(file->songs);
  if (file->author)
    free(file->author);
  if (file->misc_info)
    free(file->misc_info);
  if (file->file_data)
    free(file->file_data);
  free(file);
}

slopay_loader_song_t *slopay_loader_load_song(slopay_loader_file_t *file, uint8_t song_index)
{
  slopay_loader_song_t *song;
  size_t data_offset;
  int block_idx = 0;

  if (file == NULL || song_index >= file->num_songs)
    return NULL;

  song = malloc(sizeof(*song));
  if (song == NULL)
    return NULL;

  memset(song, 0, sizeof(*song));

  /* Read song name */
  song->name = slopay_loader_read_string(file->file_data, file->file_size,
                            file->songs[song_index].p_song_name,
                            file->songs[song_index]._struct_offset, 256);

  printf("Loading song %d: %s\n", song_index, song->name ? song->name : "(unnamed)");

  /* Get song data offset - offsets are relative to the pointer field itself. */
  size_t song_struct_offset = file->songs[song_index]._struct_offset;
  data_offset = rel_ptr(song_struct_offset + 2, file->songs[song_index].p_song_data);

  if (data_offset + 14 > file->file_size) {
    fprintf(stderr, "Error: Invalid song data offset\n");
    slopay_loader_song_destroy(song);
    return NULL;
  }

  /* Parse EMUL song data */
  song->song_data.a_chan = file->file_data[data_offset];
  song->song_data.b_chan = file->file_data[data_offset + 1];
  song->song_data.c_chan = file->file_data[data_offset + 2];
  song->song_data.noise_chan = file->file_data[data_offset + 3];
  song->song_data.song_length = read_be16(&file->file_data[data_offset + 4]);
  song->song_data.fade_length = read_be16(&file->file_data[data_offset + 6]);
  song->song_data.hi_reg = file->file_data[data_offset + 8];
  song->song_data.lo_reg = file->file_data[data_offset + 9];
  song->song_data.p_points = read_be16_signed(&file->file_data[data_offset + 10]);
  song->song_data.p_addresses = read_be16_signed(&file->file_data[data_offset + 12]);

  printf("Song length: %d/50s, Fade: %d/50s\n",
         song->song_data.song_length, song->song_data.fade_length);

  /* Load pointers */
  if (song->song_data.p_points != 0) {
    size_t points_offset;
    points_offset = rel_ptr(data_offset + 10, song->song_data.p_points);

    if (points_offset + 6 <= file->file_size) {
      song->pointers.stack = read_be16(&file->file_data[points_offset]);
      song->pointers.init = read_be16(&file->file_data[points_offset + 2]);
      song->pointers.interrupt = read_be16(&file->file_data[points_offset + 4]);
      printf("Stack: %04X, INIT: %04X, INTERRUPT: %04X\n",
             song->pointers.stack, song->pointers.init, song->pointers.interrupt);
    }
  }

  /* Load data blocks */
  if (song->song_data.p_addresses != 0) {
    size_t blocks_pos;
    blocks_pos = rel_ptr(data_offset + 12, song->song_data.p_addresses);

    /* Count blocks first */
    size_t temp_pos = blocks_pos;
    while (temp_pos + 6 <= file->file_size) {
      uint16_t addr = read_be16(&file->file_data[temp_pos]);
      if (addr == 0)
        break;
      block_idx++;
      temp_pos += 6;
    }

    if (block_idx > 0) {
      song->blocks = malloc((block_idx + 1) * sizeof(slopay_loader_data_block_t));
      if (song->blocks) {
        block_idx = 0;
        temp_pos = blocks_pos;
        while (temp_pos + 6 <= file->file_size) {
          song->blocks[block_idx].address = read_be16(&file->file_data[temp_pos]);
          if (song->blocks[block_idx].address == 0)
            break;
          song->blocks[block_idx].length = read_be16(&file->file_data[temp_pos + 2]);
          song->blocks[block_idx].offset = read_be16_signed(&file->file_data[temp_pos + 4]);
          song->blocks[block_idx]._offset_base = temp_pos + 4;
          block_idx++;
          temp_pos += 6;
        }
        song->blocks[block_idx].address = 0; /* Terminator */
      }
    }
  }

  return song;
}

void slopay_loader_song_destroy(slopay_loader_song_t *song)
{
  if (song == NULL)
    return;

  if (song->name)
    free(song->name);
  if (song->blocks)
    free(song->blocks);
  free(song);
}

char *slopay_loader_get_author(slopay_loader_file_t *file)
{
  return file ? file->author : NULL;
}

char *slopay_loader_get_misc_info(slopay_loader_file_t *file)
{
  return file ? file->misc_info : NULL;
}

uint8_t slopay_loader_get_num_songs(slopay_loader_file_t *file)
{
  return file ? file->num_songs : 0;
}

char *slopay_loader_get_song_name(slopay_loader_file_t *file, uint8_t index)
{
  if (file == NULL || index >= file->num_songs)
    return NULL;

  return slopay_loader_read_string(file->file_data, file->file_size,
                      file->songs[index].p_song_name,
                      file->songs[index]._struct_offset, 256);
}




