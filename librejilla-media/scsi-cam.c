/***************************************************************************
 *            scsi-cam.c
 *
 *  Saturday February 16, 2008
 *  Copyright  2008  Joe Marcus Clarke
 *  <marcus@FreeBSD.org>
 ****************************************************************************/

/*
 * Copyright (C) Joe Marcus Clarke 2008 <marcus@FreeBSD.org>
 *
 * Librejilla-media is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Librejilla-media authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Librejilla-media. This permission is above and beyond the permissions granted
 * by the GPL license by which Librejilla-media is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Librejilla-media is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#include <glib.h>

#include "rejilla-media-private.h"
#include "scsi-command.h"
#include "scsi-utils.h"
#include "scsi-error.h"
#include "scsi-sense-data.h"

/* FreeBSD's SCSI CAM interface */

#include <camlib.h>
#include <cam/scsi/scsi_message.h>

struct _RejillaDeviceHandle {
	struct cam_device *cam;
	int fd;
};

struct _RejillaScsiCmd {
	uchar cmd [REJILLA_SCSI_CMD_MAX_LEN];
	RejillaDeviceHandle *handle;

	const RejillaScsiCmdInfo *info;
};
typedef struct _RejillaScsiCmd RejillaScsiCmd;

#define REJILLA_SCSI_CMD_OPCODE_OFF			0
#define REJILLA_SCSI_CMD_SET_OPCODE(command)		(command->cmd [REJILLA_SCSI_CMD_OPCODE_OFF] = command->info->opcode)

#define OPEN_FLAGS			O_RDONLY /*|O_EXCL */|O_NONBLOCK

RejillaScsiResult
rejilla_scsi_command_issue_sync (gpointer command,
				 gpointer buffer,
				 int size,
				 RejillaScsiErrCode *error)
{
	int timeout;
	RejillaScsiCmd *cmd;
	union ccb cam_ccb;
	int direction = -1;

	timeout = 10;

	memset (&cam_ccb, 0, sizeof(cam_ccb));
	cmd = command;

	cam_ccb.ccb_h.path_id = cmd->handle->cam->path_id;
	cam_ccb.ccb_h.target_id = cmd->handle->cam->target_id;
	cam_ccb.ccb_h.target_lun = cmd->handle->cam->target_lun;

	if (cmd->info->direction & REJILLA_SCSI_READ)
		direction = CAM_DIR_IN;
	else if (cmd->info->direction & REJILLA_SCSI_WRITE)
		direction = CAM_DIR_OUT;

	g_assert (direction > -1);

	cam_fill_csio(&cam_ccb.csio,
		      1,
		      NULL,
		      direction,
		      MSG_SIMPLE_Q_TAG,
		      buffer,
		      size,
		      sizeof(cam_ccb.csio.sense_data),
		      cmd->info->size,
		      timeout * 1000);

	memcpy (cam_ccb.csio.cdb_io.cdb_bytes, cmd->cmd,
		REJILLA_SCSI_CMD_MAX_LEN);

	if (cam_send_ccb (cmd->handle->cam, &cam_ccb) == -1) {
		REJILLA_SCSI_SET_ERRCODE (error, REJILLA_SCSI_ERRNO);
		return REJILLA_SCSI_FAILURE;
	}

	if ((cam_ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
		REJILLA_SCSI_SET_ERRCODE (error, REJILLA_SCSI_ERRNO);
		return REJILLA_SCSI_FAILURE;
	}

	return REJILLA_SCSI_OK;
}

gpointer
rejilla_scsi_command_new (const RejillaScsiCmdInfo *info,
			  RejillaDeviceHandle *handle)
{
	RejillaScsiCmd *cmd;

	/* make sure we can set the flags of the descriptor */

	/* allocate the command */
	cmd = g_new0 (RejillaScsiCmd, 1);
	cmd->info = info;
	cmd->handle = handle;

	REJILLA_SCSI_CMD_SET_OPCODE (cmd);
	return cmd;
}

RejillaScsiResult
rejilla_scsi_command_free (gpointer cmd)
{
	g_free (cmd);
	return REJILLA_SCSI_OK;
}

/**
 * This is to open a device
 */

RejillaDeviceHandle *
rejilla_device_handle_open (const gchar *path,
			    gboolean exclusive,
			    RejillaScsiErrCode *code)
{
	int fd;
	int flags = OPEN_FLAGS;
	RejillaDeviceHandle *handle;
	struct cam_device *cam;

	g_assert (path != NULL);

/*	if (exclusive)
		flags |= O_EXCL;*/

	/* cam_open_device() fails unless we use O_RDWR */
	cam = cam_open_device (path, O_RDWR);
	fd = open (path, flags);
	if (cam && fd > -1) {
		handle = g_new0 (RejillaDeviceHandle, 1);
		handle->cam = cam;
		handle->fd = fd;
	}
	else {
		int serrno;

		if (code) {
			if (errno == EAGAIN
			||  errno == EWOULDBLOCK
			||  errno == EBUSY)
				*code = REJILLA_SCSI_NOT_READY;
			else
				*code = REJILLA_SCSI_ERRNO;
		}

		serrno = errno;

		if (fd > -1)
			close (fd);
		if (cam)
			cam_close_device (cam);

		errno = serrno;

		return NULL;
	}

	return handle;
}

void
rejilla_device_handle_close (RejillaDeviceHandle *handle)
{
	g_assert (handle != NULL);

	if (handle->cam)
		cam_close_device (handle->cam);

	close (handle->fd);

	g_free (handle);
}

char *
rejilla_device_get_bus_target_lun (const gchar *device)
{
	struct cam_device *cam_dev;
	char *addr;

	cam_dev = cam_open_device (device, O_RDWR);

	if (cam_dev == NULL) {
		REJILLA_MEDIA_LOG ("CAM: Failed to open %s: %s", device, g_strerror (errno));
		return NULL;
	}

	addr = g_strdup_printf ("%i,%i,%i", cam_dev->path_id, cam_dev->target_id, cam_dev->target_lun);

	cam_close_device (cam_dev);
	return addr;
}
