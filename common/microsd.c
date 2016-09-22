/*
 * HydraBus/HydraNFC
 *
 * Copyright (C) 2014-2015 Benjamin VERNOUX
 * Copyright (C) 2014 Bert Vermeulen <bert@biot.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <stdio.h> /* sprintf */
#include "ch.h"
#include "hal.h"

#include "ff.h"

#include "usb1cfg.h"
#include "usb2cfg.h"

#include "microsd.h"
#include "common.h"

#include "script.h"

static uint8_t outbuf[IN_OUT_BUF_SIZE+8];
static uint8_t inbuf[IN_OUT_BUF_SIZE+8];

/* FS object.*/
static FATFS SDC_FS;
/* FS Root */

/* FS mounted and ready.*/
bool fs_ready = FALSE;

char filename[FILENAME_SIZE+4] = { 0 };

#define DATA_TEST_SIZE (18)
uint8_t data[DATA_TEST_SIZE] = "Test data 12345678";

extern uint8_t* sniffer_get_buffer(void);
extern uint32_t sniffer_get_buffer_max_size(void);
extern uint32_t sniffer_get_size(void);

int mount(void);
int umount(void);
int write_file(uint8_t* buffer, uint32_t size);

bool is_fs_ready(void)
{
	return fs_ready;
}

bool is_file_present(char * filename)
{
	FRESULT err;
	if (!fs_ready) {
		err = mount();
		if(err) {
			return FALSE;
		}
	}

	err = f_stat(filename, NULL);
	if (err == FR_OK) {
		return TRUE;
	}
	return FALSE;
}

/**
 * @brief   Parody of UNIX badblocks program.
 *
 * @param[in] start       first block to check
 * @param[in] end         last block to check
 * @param[in] blockatonce number of blocks to check at once
 * @param[in] pattern     check pattern
 *
 * @return              The operation status.
 * @retval SDC_SUCCESS  operation succeeded, the requested blocks have been
 *                      read.
 * @retval SDC_FAILED   operation failed, the state of the buffer is uncertain.
 */
bool badblocks(uint32_t start, uint32_t end, uint32_t blockatonce, uint8_t pattern)
{
	uint32_t position = 0;
	uint32_t i = 0;

	chDbgCheck(blockatonce <= SDC_BURST_SIZE);

	/* fill control buffer */
	for (i=0; i < MMCSD_BLOCK_SIZE * blockatonce; i++)
		outbuf[i] = pattern;

	/* fill SD card with pattern. */
	position = start;
	while (position < end) {
		if (sdcWrite(&SDCD1, position, outbuf, blockatonce))
			goto ERROR;
		position += blockatonce;
	}

	/* read and compare. */
	position = start;
	while (position < end) {
		if (sdcRead(&SDCD1, position, inbuf, blockatonce))
			goto ERROR;
		if (memcmp(inbuf, outbuf, blockatonce * MMCSD_BLOCK_SIZE) != 0)
			goto ERROR;
		position += blockatonce;
	}
	return FALSE;

ERROR:
	return TRUE;
}

/**
 *
 */
void fillbuffer(uint8_t pattern, uint8_t *b)
{
	uint32_t i = 0;
	for (i=0; i < MMCSD_BLOCK_SIZE * SDC_BURST_SIZE; i++)
		b[i] = pattern;
}

/**
 *
 */
void fillbuffers(uint8_t pattern)
{
	fillbuffer(pattern, inbuf);
	fillbuffer(pattern, outbuf);
}

#define PRINT_PERF_VAL_DIGITS	(100)
static void print_mbs(t_hydra_console *con, uint32_t val_perf_kibs)
{
	uint32_t val;
	uint32_t val_int_part;
	uint32_t val_dec_part;

	val = val_perf_kibs / 1000;
	val_int_part = (val / (PRINT_PERF_VAL_DIGITS * 10));
	val_dec_part = val - (val_int_part * PRINT_PERF_VAL_DIGITS * 10);
	cprintf(con, "%2d.%03d", val_int_part, val_dec_part);
}

static int sd_perf_run(t_hydra_console *con, int seconds, int sectors, int offset)
{
	uint32_t n, startblk;
	systime_t start, end;

	/* The test is performed in the middle of the flash area. */
	startblk = (SDCD1.capacity / MMCSD_BLOCK_SIZE) / 2;

	start = chVTGetSystemTime();
	end = start + MS2ST(seconds * 1000);
	n = 0;
	do {
		if (blkRead(&SDCD1, startblk, g_sbuf + offset, sectors)) {
			cprintf(con, "SD read failed.\r\n");
			return FALSE;
		}
		n += sectors;
	} while (chVTIsSystemTimeWithin(start, end));

	cprintf(con, "%6d sectors/s, ", n / seconds);
	print_mbs(con, (n * MMCSD_BLOCK_SIZE) / seconds);
	cprintf(con, " MB/s\r\n");

	return TRUE;
}

#define PERFRUN_SECONDS 1
static int sd_perf(t_hydra_console *con, int offset)
{
	uint32_t nb_sectors;
	int ret;

	cprintf(con, "%sligned sequential reads:\r\n", offset ? "Una" : "A");

	/* Single block read performance. */
	cprintf(con, "0.5KiB blocks: ");
	if (!(ret = sd_perf_run(con, PERFRUN_SECONDS, 1, offset)))
		return ret;

	for(nb_sectors = 2; nb_sectors <= G_SBUF_SDC_BURST_SIZE; nb_sectors=nb_sectors*2) {
		/* Multiple sequential blocks read performance, aligned.*/
		cprintf(con, "%3dKiB blocks: ", nb_sectors/2 );
		ret = sd_perf_run(con, PERFRUN_SECONDS, nb_sectors, offset);
		if(ret == FALSE)
			return ret;
	}

	return ret;
}

int cmd_sd_test_perf(t_hydra_console *con, t_tokenline_parsed *p)
{
	(void)p;

	/* Card presence check.*/
	if (!blkIsInserted(&SDCD1)) {
		cprintf(con, "Card not inserted, aborting.\r\n");
		return FALSE;
	}

	if (sdcConnect(&SDCD1)) {
		cprintf(con, "failed.\r\n");
		return FALSE;
	}

	if (!sd_perf(con, 0))
		return FALSE;

#if STM32_SDC_SDIO_UNALIGNED_SUPPORT
	if (!sd_perf(con, 1))
		return FALSE;
#endif

	return TRUE;
}

void write_file_get_last_filename(filename_t* out_filename)
{
	out_filename->filename[0] = 0;
	sprintf(&out_filename->filename[0], "%s", filename);
}

/* Return 0 if OK else < 0 error code */
int write_file(uint8_t* buffer, uint32_t size)
{
	uint32_t i;
	FRESULT err;
	FIL FileObject;
	uint32_t bytes_written;

	if(size == 0) {
		return -1;
	}

	if(fs_ready==FALSE) {
		if(mount() != 0) {
			return -5;
		}
	} else {
		umount();
		if(mount() != 0) {
			return -5;
		}
	}

	/* Save data in file */
	for(i=0; i<999; i++) {
		sprintf(filename, "0:hydrabus_%u.txt", i);
		err = f_open(&FileObject, filename, FA_WRITE | FA_CREATE_NEW);
		if(err == FR_OK) {
			break;
		}
	}
	if(err == FR_OK) {
		err = f_write(&FileObject, buffer, size, (void *)&bytes_written);
		if(err != FR_OK) {
			f_close(&FileObject);
			umount();
			return -3;
		}

		err = f_close(&FileObject);
		if (err != FR_OK) {
			umount();
			return -4;
		}
	} else {
		umount();
		return -2;
	}

	umount();
	return 0;
}

/* return 0 if success else <0 for error */
int mount(void)
{
	FRESULT err;

	/*
	 * SDC initialization and FS mount.
	 */
	if (sdcConnect(&SDCD1)) {
		return -1;
	}

	err = f_mount(&SDC_FS, "", 0);
	if (err != FR_OK) {
		sdcDisconnect(&SDCD1);
		return -2;
	}

	fs_ready = TRUE;

	return 0;
}

/* return 0 if success else <0 for error */
int umount(void)
{
	if(!fs_ready) {
		/* File System already unmounted */
		return -1;
	}
	f_mount(NULL, "", 0);

	/* SDC Disconnect */
	sdcDisconnect(&SDCD1);
	fs_ready = FALSE;
	return 0;
}

int cmd_sd_mount(t_hydra_console *con, t_tokenline_parsed *p)
{
	FRESULT err;

	(void)p;

	if (fs_ready) {
		cprintf(con, "SD card already mounted.\r\n");
		return FALSE;
	}

	/*
	 * SDC initialization and FS mount.
	 */
	if (sdcConnect(&SDCD1)) {
		cprintf(con, "Failed to connect to SD card.\r\n");
		return FALSE;
	}

	err = f_mount(&SDC_FS, "", 0);
	if (err != FR_OK) {
		cprintf(con, "SD card mount failed.\r\n");
		sdcDisconnect(&SDCD1);
		return FALSE;
	} else {
		cprintf(con, "SD card mounted.\r\n");
	}
	fs_ready = TRUE;

	return TRUE;
}

int cmd_sd_umount(t_hydra_console *con, t_tokenline_parsed *p)
{
	(void)p;

	if(!fs_ready) {
		cprintf(con, "SD card not mounted.\r\n");
		return FALSE;
	}

	f_mount(NULL, "", 0);

	/* SDC Disconnect */
	sdcDisconnect(&SDCD1);
	fs_ready = FALSE;
	cprintf(con, "SD card unmounted.\r\n");

	return TRUE;
}

/* ls [<path>] - Directory listing */
static FRESULT sd_dir_list(t_hydra_console *con, char *path)
{
	FRESULT res;
	DIR dir;
	FILINFO fno;
	uint32_t nb_files;
	uint32_t nb_dirs;
	uint64_t file_size;
	uint32_t file_size_mb;
#if _USE_LFN
	static char lfn[_MAX_LFN + 1];   /* Buffer to store the LFN */
	fno.lfname = lfn;
	fno.lfsize = sizeof lfn;
#endif

	res = f_opendir(&dir, path);
	if(res) {
		cprintf(con, "Failed to open directory: error %d.\r\n", res);
		return res;
	}
	nb_files = 0;
	file_size = 0;
	nb_dirs = 0;

	while(1) {
		res = f_readdir(&dir, &fno);
		if ((res != FR_OK) || !fno.fname[0]) {
			break;
		}

		if (fno.fname[0] == '.') {
			continue;
		}

		if (fno.fattrib & AM_DIR) {

			nb_dirs++;
		} else {
			nb_files++;
			file_size += fno.fsize;
		}
		cprintf(con, "%c%c%c%c%c %u/%02u/%02u %02u:%02u %9lu  %s\r\n",
			(fno.fattrib & AM_DIR) ? 'd' : '-',
			(fno.fattrib & AM_RDO) ? 'r' : '-',
			(fno.fattrib & AM_HID) ? 'h' : '-',
			(fno.fattrib & AM_SYS) ? 's' : '-',
			(fno.fattrib & AM_ARC) ? 'a' : '-',
			(fno.fdate >> 9) + 1980, (fno.fdate >> 5) & 15, fno.fdate & 31,
			(fno.ftime >> 11), (fno.ftime >> 5) & 63,
			fno.fsize,
#if _USE_LFN
			*fno.lfname ? fno.lfname : fno.fname);
#else
			fno.fname);
#endif
	}

	file_size_mb = (uint32_t)(file_size/(uint64_t)(1024*1024));
	cprintf(con, "%4u File(s),%10lu MiB total %4u Dir(s)\r\n", nb_files, file_size_mb, nb_dirs);

	return res;
}

/* cd [<path>] - change directory */
int cmd_sd_cd(t_hydra_console *con, t_tokenline_parsed *p)
{
	FRESULT err;
	int offset;

	if (p->tokens[2] != T_ARG_STRING || p->tokens[4] != 0)
		return FALSE;

	memcpy(&offset, &p->tokens[3], sizeof(int));
	snprintf((char *)fbuff, FILENAME_SIZE, "0:%s", p->buf + offset);

	if (!fs_ready) {
		err = mount();
		if(err) {
			cprintf(con, "Mount failed: error %d.\r\n", err);
			return FALSE;
		}
	}

	err = f_chdir((char *)fbuff);
	if(err) {
		cprintf(con, "Failed: error %d.\r\n", err);
	}

	return TRUE;
}

/* pwd - Show current directory path */
int cmd_sd_pwd(t_hydra_console *con, t_tokenline_parsed *p)
{
	FRESULT err;

	(void)p;

	if (!fs_ready) {
		err = mount();
		if(err) {
			cprintf(con, "Mount failed: error %d.\r\n", err);
			return FALSE;
		}
	}

	err = f_getcwd((char *)fbuff, sizeof(fbuff));
	if(err) {
		cprintf(con, "Failed: error %d.\r\n", err);
		return FALSE;
	}
	cprintf(con, "%s\r\n", fbuff);

	return TRUE;
}

/* ls [<path>] - list directory */
int cmd_sd_ls(t_hydra_console *con, t_tokenline_parsed *p)
{
	FRESULT err;
	uint32_t clusters;
	FATFS *fsp;
	uint64_t free_size_bytes;
	uint32_t free_size_kb;
	uint32_t free_size_mb;

	(void)p;

	if (!fs_ready) {
		err = mount();
		if(err) {
			cprintf(con, "Mount failed: error %d.\r\n", err);
			return FALSE;
		}
	}

	if ((err = f_getcwd((char *)fbuff, sizeof(fbuff))))
		cprintf(con, "Failed to change directory: error %d.\r\n", err);

	if ((err = f_getfree("/", &clusters, &fsp)) == FR_OK) {
		free_size_bytes = (uint64_t)(clusters * SDC_FS.csize) * (uint64_t)MMCSD_BLOCK_SIZE;
		free_size_kb = (uint32_t)(free_size_bytes/(uint64_t)1024);
		free_size_mb = (uint32_t)(free_size_kb/1024);
		cprintf(con, "%lu free clusters, %lu sectors/cluster, %lu bytes/sector\r\n",
			clusters, (uint32_t)SDC_FS.csize, (uint32_t)MMCSD_BLOCK_SIZE);
		cprintf(con, "%lu KiBytes free (%lu MiBytes free)\r\n",
			free_size_kb, free_size_mb);

		err = sd_dir_list(con, (char *)fbuff);
		if (err != FR_OK) {
			cprintf(con, "Directory list failed: error %d.\r\n", err);
		}
	} else {
		cprintf(con, "Failed to get free space: error %d.\r\n", err);
	}

	return TRUE;
}

/* Call only with len=multiples of 16 (unless end of dump). */
static void dump_hexbuf(t_hydra_console *con, uint32_t offset,
			const uint8_t *buf, int len)
{
	int b, i;
	char asc[17];

	b = 0;
	while (len) {
		cprintf(con, "%.8x: ", offset + b);
		for (i = 0; i < MIN(len, 16); i++, b++) {
			cprintf(con, "%02X", buf[b]);
			if (i & 1)
				cprintf(con, " ");
			if (i == 7)
				cprintf(con, " ");
			if (buf[b] >= 0x20 && buf[b] < 0x7f)
				asc[i] = buf[b];
			else
				asc[i] = '.';
		}
		asc[i] = 0;
		cprintf(con, " %s\r\n", asc);
		len -= i;
	}
}

int cmd_sd_cat(t_hydra_console *con, t_tokenline_parsed *p)
{
#define MAX_FILE_SIZE (524288)
	bool hex;
	uint32_t str_offset, offset, filelen;
	uint32_t cnt;
	FRESULT err;
	FIL fp;

	if (p->tokens[2] != T_ARG_STRING || p->tokens[4] != 0)
		return FALSE;

	memcpy(&str_offset, &p->tokens[3], sizeof(int));
	snprintf(filename, FILENAME_SIZE, "0:%s", p->buf + str_offset);

	if (!fs_ready) {
		err = mount();
		if(err) {
			cprintf(con, "Mount failed: error %d.\r\n", err);
			return FALSE;
		}
	}

	err = f_open(&fp, filename, FA_READ | FA_OPEN_EXISTING);
	if (err != FR_OK) {
		cprintf(con, "Failed to open file %s: error %d.\r\n", filename, err);
		return FALSE;
	}

	filelen = fp.fsize;
	if(filelen > MAX_FILE_SIZE) {
		cprintf(con, "File is too big!\r\n");
		return FALSE;
	}

	hex = p->tokens[1] == T_HD;
	offset = 0;
	while(filelen) {
		if(filelen >= IN_OUT_BUF_SIZE) {
			cnt = IN_OUT_BUF_SIZE;
			filelen -= IN_OUT_BUF_SIZE;
		} else {
			cnt = filelen;
			filelen = 0;
		}
		err = f_read(&fp, inbuf, cnt, (void *)&cnt);
		if (err != FR_OK) {
			cprintf(con, "Failed to read file: error %d.\r\n", err);
			break;
		}
		if (!cnt)
			break;

		if (hex) {
			dump_hexbuf(con, offset, inbuf, cnt);
			offset += cnt;
		} else {
			cprint(con, (char *)inbuf, cnt);
		}
	}
	if (!hex)
		cprintf(con, "\r\n");

	return TRUE;
}

/**
 * SDIO Test Destructive
 */
int cmd_sd_erase(t_hydra_console *con, t_tokenline_parsed *p)
{
	uint32_t i = 0;

	if (p->tokens[2] == 0) {
		cprintf(con, "This will destroy the contents of the SD card. "
			"Confirm with 'sd erase really'.\r\n");
		return TRUE;
	} else if (p->tokens[2] != T_REALLY || p->tokens[3] != 0) {
		return FALSE;
	}

	if (!sdcConnect(&SDCD1)) {
		cprintf(con, "OK\r\n");
		cprintf(con, "*** Card CSD content is: ");
		cprintf(con, "%X %X %X %X \r\n", (&SDCD1)->csd[3], (&SDCD1)->csd[2],
			(&SDCD1)->csd[1], (&SDCD1)->csd[0]);

		cprintf(con, "Single aligned read...");
		chThdSleepMilliseconds(10);
		if (sdcRead(&SDCD1, 0, inbuf, 1)) {
			cprintf(con, "sdcRead KO\r\n");
			umount();
			return FALSE;
		}
		cprintf(con, " OK\r\n");
		chThdSleepMilliseconds(10);

		cprintf(con, "Single unaligned read...");
		chThdSleepMilliseconds(10);
		if (sdcRead(&SDCD1, 0, inbuf + 1, 1)) {
			cprintf(con, "sdcRead KO\r\n");
			umount();
			return FALSE;
		}
		if (sdcRead(&SDCD1, 0, inbuf + 2, 1)) {
			cprintf(con, "sdcRead KO\r\n");
			umount();
			return FALSE;
		}
		if (sdcRead(&SDCD1, 0, inbuf + 3, 1)) {
			cprintf(con, "sdcRead KO\r\n");
			umount();
			return FALSE;
		}
		cprintf(con, " OK\r\n");
		chThdSleepMilliseconds(10);

		cprintf(con, "Multiple aligned reads...");
		chThdSleepMilliseconds(10);
		fillbuffers(0x55);

		/* fill reference buffer from SD card */
		if (sdcRead(&SDCD1, 0, inbuf, SDC_BURST_SIZE)) {
			cprintf(con, "sdcRead KO\r\n");
			umount();
			return FALSE;
		}
		cprintf(con, "\r\n.");

		for (i=0; i<1000; i++) {
			if (sdcRead(&SDCD1, 0, outbuf, SDC_BURST_SIZE)) {
				cprintf(con, "sdcRead KO\r\n");
				umount();
				return FALSE;
			}
			if (memcmp(inbuf, outbuf, SDC_BURST_SIZE * MMCSD_BLOCK_SIZE) != 0) {
				cprintf(con, "memcmp KO\r\n");
				umount();
				return FALSE;
			}

			cprintf(con, ".");
		}
		cprintf(con, " OK\r\n");
		chThdSleepMilliseconds(10);

		cprintf(con, "Multiple unaligned reads...");
		chThdSleepMilliseconds(10);
		fillbuffers(0x55);
		/* fill reference buffer from SD card */
		if (sdcRead(&SDCD1, 0, inbuf + 1, SDC_BURST_SIZE)) {
			cprintf(con, "sdcRead KO\r\n");
			umount();
			return FALSE;
		}

		for (i=0; i<1000; i++) {
			if (sdcRead(&SDCD1, 0, outbuf + 1, SDC_BURST_SIZE)) {
				cprintf(con, "sdcRead KO\r\n");
				umount();
				return FALSE;
			}
			if (memcmp(inbuf, outbuf, SDC_BURST_SIZE * MMCSD_BLOCK_SIZE) != 0) {
				cprintf(con, "memcmp KO\r\n");
				umount();
				return FALSE;
			}
		}
		cprintf(con, " OK\r\n");
		chThdSleepMilliseconds(10);

		/* DESTRUCTIVE TEST START */
		cprintf(con, "Single aligned write...");
		chThdSleepMilliseconds(10);
		fillbuffer(0xAA, inbuf);
		if (sdcWrite(&SDCD1, 0, inbuf, 1)) {
			cprintf(con, "sdcWrite KO\r\n");
			umount();
			return FALSE;
		}
		fillbuffer(0, outbuf);
		if (sdcRead(&SDCD1, 0, outbuf, 1)) {
			cprintf(con, "sdcRead KO\r\n");
			umount();
			return FALSE;
		}
		if (memcmp(inbuf, outbuf, MMCSD_BLOCK_SIZE) != 0) {
			cprintf(con, "memcmp KO\r\n");
			umount();
			return FALSE;
		}
		cprintf(con, " OK\r\n");

		cprintf(con, "Single unaligned write...");
		chThdSleepMilliseconds(10);
		fillbuffer(0xFF, inbuf);
		if (sdcWrite(&SDCD1, 0, inbuf+1, 1)) {
			cprintf(con, "sdcWrite KO\r\n");
			umount();
			return FALSE;
		}
		fillbuffer(0, outbuf);
		if (sdcRead(&SDCD1, 0, outbuf+1, 1)) {
			cprintf(con, "sdcRead KO\r\n");
			umount();
			return FALSE;
		}
		if (memcmp(inbuf+1, outbuf+1, MMCSD_BLOCK_SIZE) != 0) {
			cprintf(con, "memcmp KO\r\n");
			umount();
			return FALSE;
		}
		cprintf(con, " OK\r\n");

		cprintf(con, "Running badblocks at 0x10000 offset...");
		chThdSleepMilliseconds(10);
		if(badblocks(0x10000, 0x11000, SDC_BURST_SIZE, 0xAA)) {
			cprintf(con, "badblocks KO\r\n");
			umount();
			return FALSE;
		}
		cprintf(con, " OK\r\n");
		/* DESTRUCTIVE TEST END */

		/**
		 * Now perform some FS tests.
		 */
		FRESULT err;
		uint32_t clusters;
		FATFS *fsp;
		FIL FileObject;
		uint32_t bytes_written;
		uint32_t bytes_read;
		uint8_t teststring[] = {"This is test file\r\n"};

		cprintf(con, "Register working area for filesystem... ");
		chThdSleepMilliseconds(10);
		err = f_mount(&SDC_FS, "", 0);
		if (err != FR_OK) {
			cprintf(con, "f_mount err:%d\r\n", err);
			umount();
			return FALSE;
		} else {
			fs_ready = TRUE;
			cprintf(con, "OK\r\n");
		}

		/* DESTRUCTIVE TEST START */
		cprintf(con, "Formatting... ");
		chThdSleepMilliseconds(10);
		err = f_mkfs("",0,0);
		if (err != FR_OK) {
			cprintf(con, "f_mkfs err:%d\r\n", err);
			umount();
			return FALSE;
		} else {
			cprintf(con, "OK\r\n");
		}
		/* DESTRUCTIVE TEST END */

		cprintf(con, "Mount filesystem... ");
		chThdSleepMilliseconds(10);
		err = f_getfree("/", &clusters, &fsp);
		if (err != FR_OK) {
			cprintf(con, "f_getfree err:%d\r\n", err);
			umount();
			return FALSE;
		}
		cprintf(con, "OK\r\n");
		cprintf(con,
			"FS: %lu free clusters, %lu sectors per cluster, %lu bytes free\r\n",
			clusters, (uint32_t)SDC_FS.csize,
			clusters * (uint32_t)SDC_FS.csize * (uint32_t)MMCSD_BLOCK_SIZE);


		cprintf(con, "Create file \"chtest.txt\"... ");
		chThdSleepMilliseconds(10);
		err = f_open(&FileObject, "0:chtest.txt", FA_WRITE | FA_OPEN_ALWAYS);
		if (err != FR_OK) {
			cprintf(con, "f_open err:%d\r\n", err);
			umount();
			return FALSE;
		}
		cprintf(con, "OK\r\n");
		cprintf(con, "Write some data in it... ");
		chThdSleepMilliseconds(10);
		err = f_write(&FileObject, teststring, sizeof(teststring), (void *)&bytes_written);
		if (err != FR_OK) {
			cprintf(con, "f_write err:%d\r\n", err);
			umount();
			return FALSE;
		} else
			cprintf(con, "OK\r\n");

		cprintf(con, "Close file \"chtest.txt\"... ");
		err = f_close(&FileObject);
		if (err != FR_OK) {
			cprintf(con, "f_close err:%d\r\n", err);
			umount();
			return FALSE;
		} else
			cprintf(con, "OK\r\n");

		cprintf(con, "Check file content \"chtest.txt\"... ");
		err = f_open(&FileObject, "0:chtest.txt", FA_READ | FA_OPEN_EXISTING);
		chThdSleepMilliseconds(10);
		if (err != FR_OK) {
			cprintf(con, "f_open err:%d\r\n", err);
			umount();
			return FALSE;
		}

		err = f_read(&FileObject, inbuf, sizeof(teststring), (void *)&bytes_read);
		if (err != FR_OK) {
			cprintf(con, "f_read KO\r\n");
			umount();
			return FALSE;
		} else {
			if (memcmp(teststring, inbuf, sizeof(teststring)) != 0) {
				cprintf(con, "memcmp KO\r\n");
				umount();
				return FALSE;
			} else {
				cprintf(con, "OK\r\n");
			}
		}

		cprintf(con, "Delete file \"chtest.txt\"... ");
		err = f_unlink("0:chtest.txt");
		if (err != FR_OK) {
			cprintf(con, "f_unlink err:%d\r\n", err);
			umount();
			return FALSE;
		}

		cprintf(con, "Umount filesystem... ");
		f_mount(NULL, "", 0);
		cprintf(con, "OK\r\n");

		cprintf(con, "Disconnecting from SDIO...");
		chThdSleepMilliseconds(10);
		if (sdcDisconnect(&SDCD1)) {
			cprintf(con, "sdcDisconnect KO\r\n");
			umount();
			return FALSE;
		}
		cprintf(con, " OK\r\n");
		cprintf(con, "------------------------------------------------------\r\n");
		cprintf(con, "All tests passed successfully.\r\n");
		chThdSleepMilliseconds(10);

		umount();
	}

	return TRUE;
}

int cmd_sd_rm(t_hydra_console *con, t_tokenline_parsed *p)
{
	FRESULT err;
	int offset;

	if (!fs_ready && (err = mount())) {
		cprintf(con, "Mount failed: error %d.\r\n", err);
		return FALSE;
	}

	memcpy(&offset, &p->tokens[3], sizeof(int));
	snprintf((char *)fbuff, FILENAME_SIZE, "0:%s", p->buf + offset);
	if ((err = f_unlink((char *)fbuff))) {
		cprintf(con, "Failed: error %d.\r\n", err);
		return FALSE;
	}

	return TRUE;
}

int cmd_sd_mkdir(t_hydra_console *con, t_tokenline_parsed *p)
{
	FRESULT err;
	int offset;

	if (!fs_ready && (err = mount())) {
		cprintf(con, "Mount failed: error %d.\r\n", err);
		return FALSE;
	}

	memcpy(&offset, &p->tokens[3], sizeof(int));
	snprintf((char *)fbuff, FILENAME_SIZE, "0:%s", p->buf + offset);
	if ((err = f_mkdir((char *)fbuff))) {
		cprintf(con, "Failed: error %d.\r\n", err);
		return FALSE;
	}

	return TRUE;
}

int cmd_sd_script(t_hydra_console *con, t_tokenline_parsed *p)
{
#define MAX_FILE_SIZE (524288)
	int str_offset;

	if (p->tokens[2] != T_ARG_STRING || p->tokens[4] != 0)
		return FALSE;

	memcpy(&str_offset, &p->tokens[3], sizeof(int));
	snprintf(filename, FILENAME_SIZE, "0:%s", p->buf + str_offset);

	execute_script(con, filename);

	return TRUE;
}

int cmd_sd(t_hydra_console *con, t_tokenline_parsed *p)
{
	int ret;

	if (p->tokens[1] == 0)
		return FALSE;

	ret = TRUE;
	switch (p->tokens[1]) {
	case T_SHOW:
		ret = cmd_show_sd(con);
		break;
	case T_MOUNT:
		ret = cmd_sd_mount(con, p);
		break;
	case T_UMOUNT:
		ret = cmd_sd_umount(con, p);
		break;
	case T_LS:
		ret = cmd_sd_ls(con, p);
		break;
	case T_CD:
		ret = cmd_sd_cd(con, p);
		break;
	case T_PWD:
		ret = cmd_sd_pwd(con, p);
		break;
	case T_CAT:
	case T_HD:
		ret = cmd_sd_cat(con, p);
		break;
	case T_ERASE:
		ret = cmd_sd_erase(con, p);
		break;
	case T_TESTPERF:
		ret = cmd_sd_test_perf(con, p);
		break;
	case T_RM:
		ret = cmd_sd_rm(con, p);
		break;
	case T_MKDIR:
		ret = cmd_sd_mkdir(con, p);
		break;
	case T_SCRIPT:
		ret = cmd_sd_script(con, p);
		break;
	default:
		return FALSE;
	}

	return ret;
}

static void print_ascii_bytes(t_hydra_console *con, uint8_t *buf, int size)
{
	bool is_ascii;
	int i;

	cprintf(con, "'");
	is_ascii = TRUE;
	for (i = 0; i < size; i++) {
		if (buf[i] < 0x20 || buf[i] > 0x7e) {
			is_ascii = FALSE;
			cprint(con, ".", 1);
		} else {
			cprint(con, (char *)buf + i, 1);
		}
	}
	cprintf(con, "'");

	if (!is_ascii) {
		for (i = 0; i < size; i++) {
			if (!i)
				cprint(con, " (", 2);
			else
				cprint(con, " ", 1);
			cprintf(con, "%02x", buf[i]);
		}
		cprintf(con, ")");
	}
	cprintf(con, "\r\n");
}

#define SHOW_SD_PADDING 23
static void print_padded(t_hydra_console *con, char *str)
{
	int size;
	const char *space = "                       ";

	size = strlen(str);
	cprint(con, str, size);
	if (size <= SHOW_SD_PADDING)
		cprint(con, space + size, SHOW_SD_PADDING - size);
	cprint(con, ": ", 2);
}

static void cmd_show_sd_cid(t_hydra_console *con, uint8_t *cid)
{
	int i;

	print_padded(con, "Manufacturer ID");
	cprintf(con, "%02x\r\n", cid[0]);

	print_padded(con, "OEM/Application ID");
	print_ascii_bytes(con, cid + 1, 2);

	print_padded(con, "Product name");
	print_ascii_bytes(con, cid + 3, 5);

	print_padded(con, "Product revision");
	cprintf(con, "%d.%d\r\n", cid[8] >> 4, cid[8] & 0x0f);

	print_padded(con, "Serial number");
	for (i = 0; i < 4; i++)
		cprintf(con, "%02x", cid[9 + i]);
	cprintf(con, "\r\n");

	print_padded(con, "Manufacturing date");
	cprintf(con, "%d-%02d\r\n", 2000 + ((cid[13] << 4) + (cid[14] >> 4)), cid[14] & 0x0f);
}

static void cmd_show_sd_csd(t_hydra_console *con, uint8_t *csd)
{
	const int taac_mul[] = { 1, 10, 100 };
	const int tran_mul[] = { 10, 100, 1000, 10000 };
	const int tran_val[] = { 0, 10, 12, 13, 15, 20, 25, 30,
				 35, 40, 45, 50, 55, 60, 70, 80
			       };
	const char *file_systems[] = {
		"Partition table", "Boot record", "Universal", "Other"
	};
	const char *write_protected[] = { "no", "temporarily",
					  "permanently", "definitely"
					};
	int csd_ver, mul, val, speed, block_len, size, tmp, i;
	char *suffix;

	csd_ver = csd[0] >> 6;

	/* Read block length, needed for capacity. */
	tmp = csd[5] & 0x0f;
	if (tmp < 9 || tmp > 11)
		block_len = 0;
	else
		block_len = 1 << tmp;

	if (csd_ver == 0) {
		tmp = ((csd[6] & 0x03) << 10) + (csd[7] << 2) + (csd[8] >> 6);
		mul = 1 << ((((csd[9] & 0x03) << 1) + (csd[10] >> 7)) + 2);
		size = (tmp + 1) * mul * block_len / 1024 / 1024;
	} else {
		tmp = ((csd[7] & 0x3f) << 16) + (csd[8] << 8) + csd[9];
		size = (tmp + 1) / 2;
	}
	print_padded(con, "Capacity");
	cprintf(con, "%d MiB\r\n", size);

	print_padded(con, "Capacity class");
	cprintf(con, "%s capacity)\r\n", csd_ver ? "HC (High" : "SC (Standard");

	print_padded(con, "TAAC");
	mul = csd[1] & 0x07;
	if (mul < 3) {
		suffix = "ns";
	} else if (mul < 6) {
		suffix = "us";
		mul -= 3;
	} else {
		suffix = "ms";
		mul -= 6;
	}
	val = (csd[1] & 0x78) >> 3;
	cprintf(con, "%d %s\r\n", val * taac_mul[mul], suffix);

	if (csd_ver == 0 && csd[2]) {
		print_padded(con, "NSAC");
		cprintf(con, "%d clock cycles\r\n", 100 * csd[2]);
	}

	print_padded(con, "Data transfer rate");
	mul = csd[3] & 0x07;
	val = (csd[3] >> 3) & 0x0f;
	if (mul > 3 || val == 0) {
		cprintf(con, "Invalid (%02x)", csd[3]);
	} else {
		speed = tran_val[val] * tran_mul[mul];
		if (mul)
			speed /= 1000;
		cprintf(con, "%d %cbit/s\r\n", speed, mul ? 'M' : 'K');
	}

	print_padded(con, "Command classes");
	tmp = (csd[4] << 4) + (csd[5] >> 4);
	for (i = 0; i < 12; i++) {
		if (tmp & (1 << i))
			cprintf(con, "%d ", i);
	}
	cprintf(con, "\r\n");

	print_padded(con, "Read block length");
	if (block_len)
		cprintf(con, "%d bytes\r\n", block_len);
	else
		cprintf(con, "Invalid (%02x)\r\n", csd[3]);

	print_padded(con, "Write block length");
	size = 1 << (((csd[12] & 0x03) << 2) + (csd[13] >> 6));
	cprintf(con, "%d bytes\r\n", size);

	print_padded(con, "Partial block reads");
	cprintf(con, "%s\r\n", csd[6] & 0x80 ? "yes" : "no");

	print_padded(con, "Partial block writes");
	cprintf(con, "%s\r\n", csd[13] & 0x20 ? "yes" : "no");

	print_padded(con, "Read-write factor");
	tmp = (csd[12] >> 2) & 0x07;
	if (tmp > 5)
		cprintf(con, "invalid (%02x)\r\n", tmp);
	else
		cprintf(con, "%d\r\n", (1 << tmp));

	print_padded(con, "Misaligned block writes");
	cprintf(con, "%s\r\n", csd[6] & 0x40 ? "yes" : "no");

	print_padded(con, "Misaligned block reads");
	cprintf(con, "%s\r\n", csd[6] & 0x20 ? "yes" : "no");

	print_padded(con, "Contents copied");
	cprintf(con, "%s\r\n", csd[14] & 0x40 ? "yes" : "no");

	print_padded(con, "Erase sector size");
	size = ((csd[10] & 0x3f) << 1) + (csd[11] >> 7) + 1;
	cprintf(con, "%d bytes\r\n", size);

	print_padded(con, "Minimum erase size");
	cprintf(con, "%d bytes\r\n", (csd[10] & 0x40) ? 512 : size);

	print_padded(con, "Write-protect possible");
	if (csd[12] & 0x80) {
		size = (csd[11] & 0x7f) + 1;
		cprintf(con, "yes (group size %d bytes)\r\n", size);
	} else {
		cprintf(con, "no\r\n");
	}

	print_padded(con, "write-protected");
	cprintf(con, "%s\r\n", write_protected[(csd[14] >> 4)]);

	if (csd_ver == 0) {
		/* FILE_FORMAT_GRP = 1 means reserved. */
		if ((csd[14] & 0x80) == 0) {
			print_padded(con, "Format");
			tmp = (csd[14] & 0x0c) >> 2;
			cprintf(con, "%s\r\n", file_systems[tmp]);
		}
	}

	print_padded(con, "Driver stage register");
	cprintf(con, "%s\r\n", csd[6] & 0x10 ? "yes" : "no");
}

int cmd_show_sd(t_hydra_console *con)
{
	int i;
	char *s;

	if (sdcConnect(&SDCD1)) {
		cprintf(con, "Failed to connect to SD card.\r\n");
		return FALSE;
	}

	print_padded(con, "Card type");
	switch (SDCD1.cardmode & SDC_MODE_CARDTYPE_MASK) {
	case SDC_MODE_CARDTYPE_SDV11:
		s = "SD 1.1";
		break;
	case SDC_MODE_CARDTYPE_SDV20:
		s = "SD 2.0";
		break;
	case SDC_MODE_CARDTYPE_MMC:
		s = "MMC";
		break;
	default:
		s = "unknown";
	}
	cprintf(con, "%s\r\n", s);

	for (i = 0; i < 16; i++)
		buf[i] = *((uint8_t *)&SDCD1.cid + (15 - i));
	cmd_show_sd_cid(con, buf);

	for (i = 0; i < 16; i++)
		buf[i] = *((uint8_t *)&SDCD1.csd + (15 - i));
	cmd_show_sd_csd(con, buf);

	print_padded(con, "Relative card address");
	cprintf(con, "%04x\r\n", SDCD1.rca >> 16);

	return TRUE;
}
