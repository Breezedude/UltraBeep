#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * SD card file system (drive 0:) and the voice pack (drive 1:).
 * FatFs is not reentrant here: every FatFs call from any task must hold the
 * (recursive) storage lock. The helpers below take it themselves.
 */
void storage_init(void);
void storage_lock(void);
/* Takes the lock only if it is free now (audio read-ahead must not wait behind a card write). */
bool storage_try_lock(void);
void storage_unlock(void);

bool storage_mount(void);
void storage_unmount(void);
bool storage_is_mounted(void);

/* Opens 0:/voices/<lang>.ubpack (or the first .ubpack) as drive 1:. */
bool storage_open_voice_pack(const char *lang);
bool storage_voice_pack_ready(void);
/* True if 1:/voices/<clip>.qoa exists. */
bool storage_voice_exists(const char *clip);
/*
 * Path of a clip: the voice pack (1:), then the extension pack voices/custom.ubpack (2:) when
 * it exists, else the single file 0:/voices/custom/<clip>.qoa. False when nowhere.
 */
#define CUSTOM_PACK_PATH "0:/voices/custom.ubpack"
#define CUSTOM_DIR       "0:/voices/custom"
bool storage_voice_locate(const char *clip, char *out, size_t size);
/* Creates a folder (true when it exists already). */
bool storage_mkdir(const char *path);
bool storage_remove(const char *path);
bool storage_exists(const char *path);
/* Streaming write of one file at a time (large files in chunks): open, chunks, close (false if any part failed). */
bool storage_write_open(const char *path);
bool storage_write_chunk(const void *data, size_t n);
bool storage_write_close(void);
/* Renames a file, replacing `to` when it exists. */
bool storage_rename(const char *from, const char *to);

/* Whole file into buf (zero-terminated); returns the length or -1. */
int storage_read_text(const char *path, char *buf, size_t size);
/* Writes a file safely: temporary file, then rename. */
bool storage_write_text(const char *path, const char *data, size_t len);

#endif
