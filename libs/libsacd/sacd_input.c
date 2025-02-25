/**
 * SACD Ripper - https://github.com/sacd-ripper/
 *
 * Copyright (c) 2010-2015 by respective authors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__lv2ppu__)
#include <sys/file.h>
#include <sys/stat.h>
#include <ppu-asm.h>

#include <sys/storage.h>
#include "ioctl.h"
#include "sac_accessor.h"
#elif defined(WIN32)
#include <io.h>
#endif

#include <utils.h>
#include <logging.h>
#include <socket.h>

#include "scarletbook.h"
#include "sacd_input.h"
#include "sacd_pb_stream.h"

sacd_input_t (*sacd_input_open)         (const char *);
int          (*sacd_input_close)        (sacd_input_t);
ssize_t      (*sacd_input_read)         (sacd_input_t, int, int, void *);
char *       (*sacd_input_error)        (sacd_input_t);
int          (*sacd_input_authenticate) (sacd_input_t);
int          (*sacd_input_decrypt)      (sacd_input_t, uint8_t *, int);
uint32_t     (*sacd_input_total_sectors)(sacd_input_t);

struct sacd_input_s
{
    int                 fd;
    uint8_t            *input_buffer;
#if defined(__lv2ppu__)
    device_info_t       device_info;
#endif
};

static int sacd_dev_input_authenticate(sacd_input_t dev)
{
#if defined(__lv2ppu__)
    int ret = create_sac_accessor();
    if (ret != 0)
    {
        LOG(lm_main, LOG_ERROR, ("create_sac_accessor (%#x)\n", ret));
        return ret;
    }

    ret = sac_exec_initialize();
    if (ret != 0)
    {
        LOG(lm_main, LOG_ERROR, ("sac_exec_initialize (%#x)\n", ret));
        return ret;
    }

    ret = sac_exec_key_exchange(dev->fd);
    if (ret != 0)
    {
        LOG(lm_main, LOG_ERROR, ("sac_exec_key_exchange (%#x)\n", ret));
        return ret;
    }
#endif
    return 0;
}

static int sacd_dev_input_decrypt(sacd_input_t dev, uint8_t *buffer, int blocks)
{
#if defined(__lv2ppu__)
    int ret, block_number = 0;
    while(block_number < blocks)
    {
        // SacModule has an internal max of 3*2048 to process
        int block_size = min(blocks - block_number, 3);     
        ret = sac_exec_decrypt_data(buffer + block_number * SACD_LSN_SIZE, block_size * SACD_LSN_SIZE, buffer + (block_number * SACD_LSN_SIZE));
        if (ret != 0)
        {
            LOG(lm_main, LOG_ERROR, ("sac_exec_decrypt_data: (%#x)\n", ret));
            return -1;
        }

        block_number += block_size;
    }
    return 0;
#elif 0
    // testing..
    int block_number = 0;
    while(block_number < blocks)
    {
        uint8_t * p = buffer + (block_number * SACD_LSN_SIZE);
        uint8_t * e = p + SACD_LSN_SIZE;
        while (p < e)
        {
            *p = 'E';
            p += 2;
        }
        block_number++;
    }
#endif
    return 0;
}

/**
 * initialize and open a SACD device or file.
 */
static sacd_input_t sacd_dev_input_open(const char *target)
{
    sacd_input_t dev;

    /* Allocate the library structure */
    dev = (sacd_input_t) calloc(sizeof(*dev), 1);
    if (dev == NULL)
    {
        fprintf(stderr, "libsacdread: Could not allocate memory.\n");
        return NULL;
    }

    /* Open the device */
#if defined(WIN32)
    dev->fd = open(target, O_RDONLY | O_BINARY);
#elif defined(__lv2ppu__)
    {
        uint8_t                 buffer[64];
        int                     ret;

        ret = sys_storage_get_device_info(BD_DEVICE, &dev->device_info);
        if (ret != 0)
        {
            goto error;
        }

        ret = sys_storage_open(BD_DEVICE, &dev->fd);
        if (ret != 0)
        {
            goto error;
        }

        memset(buffer, 0, 64);
        ioctl_get_configuration(dev->fd, buffer);
        print_hex_dump(LOG_NOTICE, "config: ", 16, 1, buffer, 64, 0);
        if ((buffer[0] & 1) != 0)
        {
            LOG(lm_main, LOG_NOTICE, ("executing ioctl_mode_sense."));
            ioctl_mode_sense(dev->fd, buffer);
            if (buffer[11] == 2)
            {
                LOG(lm_main, LOG_NOTICE, ("executing ioctl_mode_select."));
                ioctl_mode_select(dev->fd);
            }
        }
        sys_storage_get_device_info(BD_DEVICE, &dev->device_info);

        print_hex_dump(LOG_NOTICE, "device_info: ", 16, 1, &dev->device_info, sizeof(device_info_t), 0);
        if (dev->device_info.sector_size != SACD_LSN_SIZE)
        {
            LOG(lm_main, LOG_ERROR, ("incorrect LSN size [%x]\n", dev->device_info.sector_size));
            goto error;
        }

    }
#elif defined _WIN32
    dev->fd = open(target, O_RDONLY | O_BINARY);
#else
    dev->fd = open(target, O_RDONLY);
#endif
    if (dev->fd < 0)
    {
        goto error;
    }

    return dev;

error:

    free(dev);

    return 0;
}

/**
 * return the last error message
 */
static char *sacd_dev_input_error(sacd_input_t dev)
{
    /* use strerror(errno)? */
    return (char *) "unknown error";
}

/**
 * read data from the device.
 */
static ssize_t sacd_dev_input_read(sacd_input_t dev, int pos, int blocks, void *buffer)
{
#if defined(__lv2ppu__)
    int      ret;
    uint32_t sectors_read;

    ret = sys_storage_read(dev->fd, pos, blocks, buffer, &sectors_read);

    return (ret != 0) ? 0 : sectors_read;

#else
    ssize_t ret, len;

    ret = lseek(dev->fd, (off_t) pos * (off_t) SACD_LSN_SIZE, SEEK_SET);
    if (ret < 0)
    {
        return 0;
    }

    len = (size_t) blocks * SACD_LSN_SIZE;

    while (len > 0)
    {
        ret = read(dev->fd, buffer, (unsigned int) len);

        if (ret < 0)
        {
            /* One of the reads failed, too bad.  We won't even bother
             * returning the reads that went OK, and as in the POSIX spec
             * the file position is left unspecified after a failure. */
            return ret;
        }

        if (ret == 0)
        {
            /* Nothing more to read.  Return all of the whole blocks, if any.
             * Adjust the file position back to the previous block boundary. */
            ssize_t bytes     = (ssize_t) blocks * SACD_LSN_SIZE - len;
            off_t   over_read = -(bytes % SACD_LSN_SIZE);
            /*off_t pos =*/ lseek(dev->fd, over_read, SEEK_CUR);
            /* should have pos % 2048 == 0 */
            return (int) (bytes / SACD_LSN_SIZE);
        }

        len -= ret;
    }

    return blocks;
#endif
}

/**
 * close the SACD device and clean up.
 */
static int sacd_dev_input_close(sacd_input_t dev)
{
    int ret;

#if defined(__lv2ppu__)
    ret = sac_exec_exit();
    if (ret != 0)
    {
        LOG(lm_main, LOG_ERROR, ("sac_exec_exit (%#x)\n", ret));
    }

    ret = destroy_sac_accessor();
    if (ret != 0)
    {
        LOG(lm_main, LOG_ERROR, ("destroy_sac_accessor (%#x)\n", ret));
    }

    ret = sys_storage_close(dev->fd);
#else
    ret = close(dev->fd);
#endif

    free(dev);

    return ret;
}

static uint32_t sacd_dev_input_total_sectors(sacd_input_t dev)
{
    if (!dev)
        return 0;

#if defined(__lv2ppu__)
    return dev->device_info.total_sectors;
#else
    {
        struct stat file_stat;
        if(fstat(dev->fd, &file_stat) < 0)    
            return 0;

        return (uint32_t) (file_stat.st_size / SACD_LSN_SIZE);
    }
#endif
}



/**
 * Setup read functions for file access
 */
int sacd_input_setup(const char* path)
{

    sacd_input_open = sacd_dev_input_open;
    sacd_input_close = sacd_dev_input_close;
    sacd_input_read = sacd_dev_input_read;
    sacd_input_error = sacd_dev_input_error;
    sacd_input_authenticate  = sacd_dev_input_authenticate;
    sacd_input_decrypt = sacd_dev_input_decrypt;
    sacd_input_total_sectors = sacd_dev_input_total_sectors;

    return 0;
} 
