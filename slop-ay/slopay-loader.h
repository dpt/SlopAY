/* slopay-loader.h
 *
 * AY format music file loader (Motorola/big-endian format)
 */

#ifndef SLOPAY_LOADER_H
#define SLOPAY_LOADER_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
  char file_id[4];
  char type_id[4];
  uint8_t file_version;
  uint8_t player_version;
  int16_t p_special_player;
  int16_t p_author;
  int16_t p_misc;
  uint8_t num_of_songs;
  uint8_t first_song;
  int16_t p_songs_structure;
} slopay_loader_file_header_t;

typedef struct {
  int16_t p_song_name;
  int16_t p_song_data;
  size_t _struct_offset;
} slopay_loader_song_structure_t;

typedef struct {
  uint8_t a_chan;
  uint8_t b_chan;
  uint8_t c_chan;
  uint8_t noise_chan;
  uint16_t song_length;
  uint16_t fade_length;
  uint8_t hi_reg;
  uint8_t lo_reg;
  int16_t p_points;
  int16_t p_addresses;
} slopay_loader_song_data_emul_t;

typedef struct {
  uint16_t stack;
  uint16_t init;
  uint16_t interrupt;
} slopay_loader_pointers_t;

typedef struct {
  uint16_t address;
  uint16_t length;
  int16_t offset;
  size_t _offset_base;
} slopay_loader_data_block_t;

typedef struct {
  uint8_t *file_data;
  size_t file_size;
  slopay_loader_file_header_t header;
  char *author;
  char *misc_info;
  uint8_t num_songs;
  slopay_loader_song_structure_t *songs;
} slopay_loader_file_t;

typedef struct {
  char *name;
  slopay_loader_song_data_emul_t song_data;
  slopay_loader_pointers_t pointers;
  slopay_loader_data_block_t *blocks;
  uint8_t z80_memory[65536];
  uint16_t current_pc;
  uint32_t cycles_count;
} slopay_loader_song_t;

slopay_loader_file_t *slopay_loader_load_file(const char *filename);
void slopay_loader_file_destroy(slopay_loader_file_t *file);
slopay_loader_song_t *slopay_loader_load_song(slopay_loader_file_t *file, uint8_t song_index);
void slopay_loader_song_destroy(slopay_loader_song_t *song);
char *slopay_loader_get_author(slopay_loader_file_t *file);
char *slopay_loader_get_misc_info(slopay_loader_file_t *file);
uint8_t slopay_loader_get_num_songs(slopay_loader_file_t *file);
char *slopay_loader_get_song_name(slopay_loader_file_t *file, uint8_t index);

#endif /* SLOPAY_LOADER_H */

