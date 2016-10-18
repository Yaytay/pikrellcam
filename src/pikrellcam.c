/* PiKrellCam
|
|  Copyright (C) 2015-2016 Bill Wilson    billw@gkrellm.net
|
|  PiKrellCam is free software: you can redistribute it and/or modify it
|  under the terms of the GNU General Public License as published by
|  the Free Software Foundation, either version 3 of the License, or
|  (at your option) any later version.
|
|  PiKrellCam is distributed in the hope that it will be useful, but WITHOUT
|  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
|  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
|  License for more details.
|
|  You should have received a copy of the GNU General Public License
|  along with this program. If not, see http://www.gnu.org/licenses/
|
|  This file is part of PiKrellCam.
*/

#include "pikrellcam.h"
#include <signal.h>
#include <locale.h>
#include <sys/statvfs.h>


PiKrellCam	pikrellcam;
TimeLapse	time_lapse;

static char	*pgm_name;
static boolean	quit_flag;

static uid_t  user_uid;
static gid_t  user_gid;
static char   *homedir;

  /* Substitute fmt_arg into str to replace a "$V" substitution variable. 
  |  "str" argument must be allocated memory.
  */
char *
substitute_var(char *str, char V, char *fmt_arg)
	{
	char	*fmt, *copy;

	if (V < 'A' || !fmt_arg)
		return str;
	for (fmt = str; (fmt = strchr(fmt, '$')) != NULL; ++fmt)
		{
	    if (*(fmt + 1) == V)
			{
			*fmt = '%';
			*(fmt + 1) = 's';
			asprintf(&copy, str, fmt_arg);
			free(str);
			str = copy;
			break;
			}
		}
	return str;
	}

  /* Make a media pathname out of a filename which can have embedded strftime()
  |  conversions and up to two pikrellcam filename substitution variables.
  |  Return string is allocated memory.
  */
char *
media_pathname(char *dir, char *fname, time_t time,
				char var1, char *arg1, char var2, char *arg2)
	{
	struct tm	tm;
	char		buf[200], *path;

	if (time == 0)
		time = pikrellcam.t_now;
	localtime_r(&time, &tm);

	/* Do strftime() first to get rid of '%' specifiers in fname.
	*/
	buf[0] = '\0';
	strftime(buf, sizeof(buf), fname, &tm);
	asprintf(&path, "%s/%s", dir, buf);

	/* Now process any $V substitution variables.
	*/
	path = substitute_var(path, var1, arg1);
	path = substitute_var(path, var2, arg2);
	return path;
	}

char *
fname_base(char *path)
	{
	char *s, *slash;

	for (s = path, slash = NULL; *s; ++s)
		if (*s == '/')
			slash = s;
	return slash ? slash + 1 : path;
	}

void
log_printf_no_timestamp(char *fmt, ...)
	{
	va_list	args;
	FILE	*f;

	va_start(args, fmt);
	if (pikrellcam.verbose)
		vfprintf(stderr, fmt, args);
	if ((f = fopen(pikrellcam.log_file, "a")) != NULL)
		{
		vfprintf(f, fmt, args);
		fclose(f);
		}
	va_end(args);
	}

void
log_printf(char *fmt, ...)
	{
	va_list	args;
	FILE	*f;
	char	tbuf[32];

	va_start(args, fmt);
	strftime(tbuf, sizeof(tbuf), "%T", localtime(&pikrellcam.t_now));
	if (pikrellcam.verbose)
		{
		fprintf(stderr, "%s : ", tbuf);
		vfprintf(stderr, fmt, args);
		}
	if ((f = fopen(pikrellcam.log_file, "a")) != NULL)
		{
		fprintf(f, "%s : ", tbuf);
		vfprintf(f, fmt, args);
		fclose(f);
		}
	va_end(args);
	}

void
annotate_string_add(char *cmd, char *id, char *str)
	{
	AnnotateString *annotate;
	SList          *list;
	char           *c;

	for (list = pikrellcam.annotate_list; list; list = list->next)
		{
		annotate = (AnnotateString *) list->data;
		if (!strcmp(annotate->id, id))
			break;
		}
	if (!list)
		{
		annotate = calloc(1, sizeof(AnnotateString));
		annotate->id = strdup(id);
		pikrellcam.annotate_list = slist_append(pikrellcam.annotate_list, annotate);
		}
	annotate->prepend = (strcmp(cmd, "prepend") == 0);
	dup_string(&annotate->string, str);
	for (c = annotate->string; *c != '\0'; ++c)
		if (*c == pikrellcam.annotate_string_space_char)
			*c = ' ';
	}

void
annotate_string_remove(char *id)
	{
	AnnotateString *annotate;
	SList          *list;

	for (list = pikrellcam.annotate_list; list; list = list->next)
		{
		annotate = (AnnotateString *) list->data;
		if (!strcmp(annotate->id, id))
			{
			pikrellcam.annotate_list = slist_remove(pikrellcam.annotate_list, annotate);
			free(annotate->id);
			free(annotate->string);
			free(annotate);
			break;
			}
		}
	}

void
camera_start(void)
	{
	MMAL_STATUS_T status;
	char          *cmd;

	motion_init();
	circular_buffer_init();

	if (!camera_create())
		{
		pikrellcam.verbose = TRUE;
		log_printf("Failed to start the camera.  Possible causes:\n");
		log_printf("   Another program is using the camera.\n");
		log_printf("   Camera ribbon cable connection problem.\n");
		fprintf(stderr, "See /tmp/pikrellcam.log for an error message.\n");
		asprintf(&cmd, "cp %s/www/images/camera-error.jpg %s",
				pikrellcam.install_dir, pikrellcam.mjpeg_filename);
		exec_wait(cmd, NULL);
		exit(1);
		}

	/* ====== Create the camera preview port path ====== :
	|  preview --(tunnel)--> resizer --> I420_callback --> jpeg_encoder --> mjpeg_callback
	|                                   (draws on frame)                   (writes stream mjpeg.jpg)
	*/
	pikrellcam.mjpeg_height = pikrellcam.mjpeg_width *
					pikrellcam.camera_config.video_height / pikrellcam.camera_config.video_width;
	pikrellcam.mjpeg_width &= ~0xf;		/* Make resize multiple of 16 */
	pikrellcam.mjpeg_height &= ~0xf;
	resizer_create("stream_resizer", &stream_resizer,
							camera.component->output[CAMERA_PREVIEW_PORT],
							pikrellcam.mjpeg_width, pikrellcam.mjpeg_height);
	ports_tunnel_connect(&camera, CAMERA_PREVIEW_PORT, &stream_resizer);
	jpeg_encoder_create("mjpeg_encoder", &mjpeg_encoder,
					stream_resizer.component->output[0], pikrellcam.mjpeg_quality);
	ports_callback_connect(&stream_resizer, 0, &mjpeg_encoder,
					I420_video_callback);
	out_port_callback(&mjpeg_encoder, 0, mjpeg_callback);


	/* ====== Create the camera still port path ====== :
	|  camera_capture --(tunnel)--> jpeg_encoder --> still_jpeg__callback
	|                                               (writes stills and timelapse jpegs)
	*/
	jpeg_encoder_create("still_jpeg_encoder", &still_jpeg_encoder,
					NULL, pikrellcam.camera_adjust.still_quality);
	ports_tunnel_connect(&camera, CAMERA_CAPTURE_PORT, &still_jpeg_encoder);
	out_port_callback(&still_jpeg_encoder, 0, still_jpeg_callback);


	/* ====== Create the camera video port path ====== :
	|  camera_video--(tunnel)-->h264 encoder-->video_h264_encoder_callback
	|                                         (writes data into video circular buffer)
	|                                         (records video / checks motion vectors)
	|                                         (schedules mjpeg.jpg copy into previews)
	*/
	h264_encoder_create("video_h264_encoder", &video_h264_encoder, NULL);
	ports_tunnel_connect(&camera, CAMERA_VIDEO_PORT, &video_h264_encoder);
	out_port_callback(&video_h264_encoder, 0, video_h264_encoder_callback);

	time(&pikrellcam.t_start);

	/* Turn on the video stream. It free runs into the video circular buffer.
	*/
	if ((status = mmal_port_parameter_set_boolean(
				camera.component->output[CAMERA_VIDEO_PORT],
				MMAL_PARAMETER_CAPTURE, 1)) != MMAL_SUCCESS)
		log_printf("Video capture startup failed. Status %s\n",
					mmal_status[status]);

	/* With everything created and running, set the config'ed camera params.
	*/
	mmalcam_config_parameters_set_camera();

	display_init();
	video_circular_buffer.state = VCB_STATE_NONE;
	video_circular_buffer.pause = FALSE;
	pikrellcam.state_modified = TRUE;
	}


void
camera_stop(void)
	{
	mmal_port_parameter_set_boolean(camera.component->output[CAMERA_VIDEO_PORT],
				MMAL_PARAMETER_CAPTURE, 0);

	camera_object_destroy(&video_h264_encoder);
	camera_object_destroy(&still_jpeg_encoder);
	camera_object_destroy(&stream_resizer);
	camera_object_destroy(&mjpeg_encoder);
	camera_object_destroy(&camera);
	}

void
camera_restart(void)
	{
	VideoCircularBuffer	*vcb = &video_circular_buffer;

	pthread_mutex_lock(&vcb->mutex);
	video_record_stop(vcb);
	vcb->state = VCB_STATE_RESTARTING;
	pikrellcam.camera_adjust = camera_adjust_temp;	/* May not be changed */
	pthread_mutex_unlock(&vcb->mutex);

	camera_stop();
	camera_start();
	}

boolean
still_capture(char *fname)
	{
	Event			*event;
	int				n;
	MMAL_STATUS_T	status;
	boolean			result = FALSE;

	/* timelapse_shapshot() also uses the still_jpeg_encoder, so wait if busy.
	*/
	for (n = 0; n < 10; ++n)
		{
		if (still_jpeg_encoder.file == NULL)
			break;
		usleep(50000);
		}
	if (still_jpeg_encoder.file != NULL)
		{
		/* inform() */
		log_printf("still capture failed because jpeg encoder is busy.\n");
		return FALSE;
		}

	if ((still_jpeg_encoder.file = fopen(fname, "w")) == NULL)
		log_printf("Could not create still file %s.  %m\n", fname);
	else
		{
		if ((status = mmal_port_parameter_set_boolean(
						camera.component->output[CAMERA_CAPTURE_PORT],
						MMAL_PARAMETER_CAPTURE, 1)) != MMAL_SUCCESS)
			{
			fclose(still_jpeg_encoder.file);
			still_jpeg_encoder.file = NULL;
			log_printf("Still capture startup failed. Status %s\n",
						mmal_status[status]);
			}
		else
			{
			result = TRUE;
			log_printf("Still: %s\n", fname);
			dup_string(&pikrellcam.still_last, fname);
			n = pikrellcam.notify_duration * EVENT_LOOP_FREQUENCY;

			event_list_lock();
			if ((event = event_find("still saved")) != NULL)
				event->count = n;	/* rapid stills, extend the time */
			event_list_unlock();
			if (!event)
				event_count_down_add("still saved", n,
						event_notify_expire, &pikrellcam.still_notify);
			pikrellcam.still_capture_event = TRUE;
			pikrellcam.still_notify = TRUE;
			}
		}
	return result;
	}

void
timelapse_capture(void)
	{
	char			*path, seq_buf[12], series_buf[12];
	int				n, nd;
	MMAL_STATUS_T	status;

	if (time_lapse.on_hold)
		return;

	/* still_capture()  also uses the still_jpeg_encoder, so wait if busy.
	*/
	for (n = 0; n < 10; ++n)
		{
		if (still_jpeg_encoder.file == NULL)
			break;
		usleep(50000);
		}
	if (still_jpeg_encoder.file != NULL)
		{
		log_printf("timelapse capture failed because jpeg encoder is busy.\n");
		return;
		}

	snprintf(seq_buf, sizeof(seq_buf), "%05d", time_lapse.sequence);
	snprintf(series_buf, sizeof(series_buf), "%05d", time_lapse.series);
	path = media_pathname(pikrellcam.timelapse_dir,
						pikrellcam.timelapse_format, 0,
						'N',  seq_buf,
						'n',  series_buf);

	if ((still_jpeg_encoder.file = fopen(path, "w")) == NULL)
		log_printf("Could not create timelapse file %s.  %m\n", path);
	else
		{
		dup_string(&pikrellcam.timelapse_jpeg_last, path);
		pikrellcam.timelapse_capture_event = TRUE;
		if ((status = mmal_port_parameter_set_boolean(
						camera.component->output[CAMERA_CAPTURE_PORT],
						MMAL_PARAMETER_CAPTURE, 1)) != MMAL_SUCCESS)
			{
			fclose(still_jpeg_encoder.file);
			unlink(path);
			still_jpeg_encoder.file = NULL;
			dup_string(&pikrellcam.timelapse_jpeg_last, "failed");
			pikrellcam.timelapse_capture_event = FALSE;
			log_printf("Timelapse capture startup failed. Status %s\n",
						mmal_status[status]);
			}
		else
			{
			log_printf("Timelapse still: %s\n", path);

			/* timelapse_capture() is an event call (inside the event loop)
			|  and we here add an event to the list.
			|  This only modifies the last event list next pointer, and
			|  the event loop is not there yet.
			*/
			nd = pikrellcam.notify_duration;
			if (nd > time_lapse.period - 3)
				nd = time_lapse.period / 2;
			if (nd > 1)
				nd *= EVENT_LOOP_FREQUENCY;
			else
				nd = EVENT_LOOP_FREQUENCY / 2;

			event_count_down_add("timelapse saved", nd,
					event_notify_expire, &pikrellcam.timelapse_notify);
			pikrellcam.timelapse_notify = TRUE;
			}
		}
	free(path);
	}

void
timelapse_inform_convert(void)
	{
	struct stat st;
	char		fname[200];

	if (!time_lapse.convert_name || !*time_lapse.convert_name)
		return;
	snprintf(fname, sizeof(fname), "%s/%s.mp4",
			pikrellcam.timelapse_dir, time_lapse.convert_name);
	st.st_size = 0;
	stat(fname, &st);
	time_lapse.convert_size = st.st_size;
	}

  /* vcb should be locked before calling video_record_start()
  */
void
video_record_start(VideoCircularBuffer *vcb, int start_state)
	{
	MotionFrame	*mf = &motion_frame;
	time_t		t_cur = pikrellcam.t_now;
	int			n;
	char		*s, *path, *stats_path = NULL, seq_buf[12];
	boolean		do_stats = FALSE;

	if (vcb->state == VCB_STATE_MANUAL_RECORD)
		return;

	if (start_state == VCB_STATE_MOTION_RECORD_START)
		{
		if (mf->external_trigger_pre_capture > 0)
			{
			n = vcb->cur_frame_index;
			if (mf->external_trigger_pre_capture > vcb->seconds - 1)
				mf->external_trigger_pre_capture = vcb->seconds - 1;
			while (t_cur - vcb->key_frame[n].t_frame < mf->external_trigger_pre_capture)
				{
				if (vcb->key_frame[n].t_frame == 0)
					{
					n = (n + 1) % KEYFRAME_SIZE;
					break;
					}
				if (--n < 0)
					n = KEYFRAME_SIZE - 1;
				if (n == vcb->cur_frame_index)
					break;
				}
			if (t_cur - vcb->key_frame[n].t_frame > mf->external_trigger_pre_capture)
				n = (n + 1) % KEYFRAME_SIZE;
			}
		else
			n = vcb->pre_frame_index;

		vcb->record_start_time = vcb->key_frame[n].t_frame;
		vcb->record_elapsed_time = t_cur - vcb->record_start_time;
		vcb->record_start_frame_index = n;
		pikrellcam.video_start_pts = vcb->key_frame[n].frame_pts;

		snprintf(seq_buf, sizeof(seq_buf), "%d",
					pikrellcam.video_motion_sequence);
		path = media_pathname(pikrellcam.video_dir,
					pikrellcam.video_motion_name_format,
					vcb->record_start_time,
					'N',  seq_buf,
					'H', pikrellcam.hostname);
		pikrellcam.video_motion_sequence += 1;
		if (pikrellcam.motion_stats)
			do_stats = TRUE;
		}
	else
		{
		n = vcb->cur_frame_index;
		if (vcb->manual_pre_capture > vcb->seconds - 1)
			vcb->manual_pre_capture = vcb->seconds - 1;
		while (t_cur - vcb->key_frame[n].t_frame < vcb->manual_pre_capture)
			{
			if (vcb->key_frame[n].t_frame == 0)
				{
				n = (n + 1) % KEYFRAME_SIZE;
				break;
				}
			if (--n < 0)
				n = KEYFRAME_SIZE - 1;
			if (n == vcb->cur_frame_index)
				break;
			}
		if (t_cur - vcb->key_frame[n].t_frame > vcb->manual_pre_capture)
			n = (n + 1) % KEYFRAME_SIZE;

		vcb->record_start_time = vcb->key_frame[n].t_frame;
		vcb->record_elapsed_time = t_cur - vcb->record_start_time;
		vcb->record_start_frame_index = n;
		pikrellcam.video_start_pts = vcb->key_frame[n].frame_pts;

		snprintf(seq_buf, sizeof(seq_buf), "%d",
					pikrellcam.video_manual_sequence);
		path = media_pathname(pikrellcam.video_dir,
					pikrellcam.video_manual_name_format,
					vcb->record_start_time,
					'N',  seq_buf,
					'H', pikrellcam.hostname);
		pikrellcam.video_manual_sequence += 1;
		}

	dup_string(&pikrellcam.video_pathname, path);
	free(path);
	path = pikrellcam.video_pathname;

	if ((s = strstr(path, ".mp4")) != NULL && *(s + 4) == '\0')
		{
		if (do_stats)
			{
			*s = '\0';
			asprintf(&stats_path, "%s.csv", path);
			*s = '.';
			}
		asprintf(&path, "%s.h264", pikrellcam.video_pathname);
		dup_string(&pikrellcam.video_h264, path);
		free(path);
		path = pikrellcam.video_h264;
		pikrellcam.video_mp4box = TRUE;
		}
	else
		pikrellcam.video_mp4box = FALSE;

	if ((vcb->file = fopen(path, "w")) == NULL)
		log_printf("Could not create video file %s.  %m\n", path);
	else
		{
		log_printf("Video record: %s ...\n", path);
		vcb->state = start_state;
		pikrellcam.state_modified = TRUE;
		if (   do_stats
		    && (vcb->motion_stats_file = fopen(stats_path, "w")) != NULL
		   )
			vcb->motion_stats_do_header = TRUE;
		}
	}

  /* vcb should be locked before calling video_record_stop()
  */
void
video_record_stop(VideoCircularBuffer *vcb)
	{
	struct statvfs st;
	struct stat    st_h264;
	MotionFrame    *mf = &motion_frame;
	unsigned long  tmp_space;
	Event          *event = NULL;
	char           *s, *cmd, *tmp_dir, *detect, *thumb_name, *thumb_cmd = NULL;
	int            thumb_height;

	if (!vcb->file)
		return;

	fclose(vcb->file);
	vcb->file = NULL;
	if (vcb->motion_stats_file)
		{
		fclose(vcb->motion_stats_file);
		vcb->motion_stats_file = NULL;
		}
	log_printf("Video %s record stopped. Header size: %d  h264 file size: %d\n",
			(vcb->state & VCB_STATE_MOTION_RECORD) ? "motion" : "manual",
			pikrellcam.video_header_size, pikrellcam.video_size);
	if (vcb->state & VCB_STATE_MOTION_RECORD)
		{
		if ((mf->first_detect & (MOTION_BURST | MOTION_DIRECTION))
				== (MOTION_BURST | MOTION_DIRECTION))
			detect = "both";
		else if (mf->first_detect & MOTION_BURST)
			detect = "burst";
		else
			detect = "direction";
		log_printf(
"    first detect: %s  totals - direction: %d  burst: %d  max burst count: %d\n",
				detect,
				mf->direction_detects, mf->burst_detects, mf->max_burst_count);
		}

	if (pikrellcam.verbose_motion && !pikrellcam.verbose)
		printf("***Motion record stop: %s\n", pikrellcam.video_pathname);

	st_h264.st_size = 0;
	stat(pikrellcam.video_h264, &st_h264);

	if (pikrellcam.video_mp4box)
		{
		statvfs("/tmp", &st);
		tmp_space = st.f_bfree * st.f_frsize;

		if (tmp_space > 4 * (unsigned long) st_h264.st_size / 3)
			tmp_dir = "/tmp";
		else
			tmp_dir = pikrellcam.video_dir;

		if (vcb->state & VCB_STATE_MANUAL_RECORD)
			{
			thumb_height = 150 * pikrellcam.camera_config.video_height
							/ pikrellcam.camera_config.video_width;
			asprintf(&thumb_name, "%s/%s   ", pikrellcam.thumb_dir,
					fname_base(pikrellcam.video_pathname));
			if ((s = strstr(thumb_name, ".mp4")) != NULL)
				strcpy(s, ".th.jpg");

			asprintf(&thumb_cmd, "&& avconv -i %s -ss 0 -s 150x%d %s",
				pikrellcam.video_pathname, thumb_height,
				thumb_name);
			free(thumb_name);
			}

		if (st_h264.st_size > 0)  // can be 0 if no space left
			asprintf(&cmd, "(MP4Box %s -tmp %s -fps %d -add %s %s %s && rm %s %s)",
				pikrellcam.verbose ? "" : "-quiet",
				tmp_dir,
				(pikrellcam.camera_adjust.video_mp4box_fps > 0) ?
						pikrellcam.camera_adjust.video_mp4box_fps :
						pikrellcam.camera_adjust.video_fps,
				pikrellcam.video_h264, pikrellcam.video_pathname,
				pikrellcam.verbose ? "" : "2> /dev/null",
				pikrellcam.video_h264,
				thumb_cmd ? thumb_cmd : "");
		else
			asprintf(&cmd, "rm %s", pikrellcam.video_h264);

		if (thumb_cmd)
			free(thumb_cmd);

		if (   (vcb->state & VCB_STATE_MOTION_RECORD)
		    && *pikrellcam.on_motion_end_cmd
		    && st_h264.st_size > 0
		   )
			event = exec_child_event("motion end command", cmd, NULL);
		else
			exec_no_wait(cmd, NULL);
		free(cmd);
		}
	dup_string(&pikrellcam.video_last, pikrellcam.video_pathname);
	pikrellcam.video_last_frame_count = vcb->video_frame_count;
	pikrellcam.video_end_pts = vcb->last_pts;

	pikrellcam.state_modified = TRUE;

	pikrellcam.video_notify = TRUE;
	event_count_down_add("video saved notify",
				pikrellcam.notify_duration * EVENT_LOOP_FREQUENCY,
				event_notify_expire, &pikrellcam.video_notify);
	if (vcb->state & VCB_STATE_MOTION_RECORD)
		{
		if (!strcmp(pikrellcam.motion_preview_save_mode, "best"))
			{
			motion_preview_area_fixup();
			event_add("motion area thumb", pikrellcam.t_now, 0,
					event_motion_area_thumb, NULL);
			event_add("preview save command", pikrellcam.t_now, 0,
					event_preview_save_cmd,
					pikrellcam.on_motion_preview_save_cmd);
			}
		if (event)	/* a mp4 video save event needs a MP4Box child exit */
			{
			event->data = pikrellcam.on_motion_end_cmd;
			event->func = event_motion_end_cmd;
			}
		else if (   *pikrellcam.on_motion_end_cmd	/* a h264 video save */
		         && st_h264.st_size > 0
		        )
			event_add("motion end command", pikrellcam.t_now, 0,
					event_motion_end_cmd, pikrellcam.on_motion_end_cmd);
		}
	event_add("preview dispose", pikrellcam.t_now, 0,
					event_preview_dispose, NULL);

	mf->external_trigger_mode = EXT_TRIG_MODE_DEFAULT;
	mf->external_trigger_pre_capture = 0;
	mf->external_trigger_time_limit = 0;

	vcb->state = VCB_STATE_NONE;
	motion_event_write(vcb, mf);
	pikrellcam.state_modified = TRUE;
	vcb->pause = FALSE;
	}

static boolean
get_arg_pass1(char *arg)
	{
	if (!strcmp(arg, "-quit"))
		quit_flag = TRUE;

	if (!strcmp(arg, "-V") || !strcmp(arg, "--version"))
		{
		printf("%s\n", PIKRELLCAM_VERSION);
		exit(0);
		}
	else if (!strcmp(arg, "-h") || !strcmp(arg, "--help"))
		{
		/* XXX */
		exit(0);
		}
	else if (!strcmp(arg, "-v"))
		pikrellcam.verbose = TRUE;
	else if (!strcmp(arg, "-vm"))
		pikrellcam.verbose_motion = TRUE;
	else if (!strcmp(arg, "-vmulti"))
		pikrellcam.verbose_multicast = TRUE;
	else if (!strcmp(arg, "-debug"))
		pikrellcam.debug = TRUE;
	else if (!strcmp(arg, "-debug-fps"))
		pikrellcam.debug_fps = TRUE;
	else if (!strncmp(arg, "-user", 5))
		user_uid = atoi(arg + 5);
	else if (!strncmp(arg, "-group", 6))
		user_gid = atoi(arg + 6);
	else if (!strncmp(arg, "-home", 5))
		homedir = strdup(arg + 5);
	else
		return FALSE;
	return TRUE;
	}

static void
pikrellcam_cleanup(void)
	{
	config_timelapse_save_status();
	preset_state_save();
	if (pikrellcam.config_modified)
		config_save(pikrellcam.config_file);
	if (pikrellcam.preset_modified)
		preset_config_save();
	}

static void
signal_quit(int sig)
	{
	pikrellcam_cleanup();
	display_quit();
	log_printf("quit signal received - exiting!\n");
	exit(0);
	}


typedef enum
	{
	record,
	record_pause,
	still,

	tl_start,
	tl_end,
	tl_hold,
	tl_inform_convert,
	tl_show_status,

	motion_cmd,
	motion_enable,
	display_cmd,        /* Placement above here can affect OSD.  If menu */
		                /* or adjustment is showing, above commands redirect */
	                    /* to cancel the menu or adjustment. */
	still_quality,
	video_bitrate,
	video_fps,
	video_mp4box_fps,
	inform,
	save_config,
	archive_video,
	archive_still,
	annotate_string,
	delete_log,
	fix_thumbs,
	servo_cmd,
	preset_cmd,
	multicast,
	upgrade,
	halt,
	reboot,
	quit
	}
	CommandCode;
	
typedef struct
	{
	char		*name;
	CommandCode	code;
	int			n_args;
	boolean     do_log;
	}
	Command;

static Command commands[] =
	{
	{ "record",      record,        1, TRUE },
	{ "record_pause", record_pause, 0, TRUE },
	{ "pause",       record_pause,  0, TRUE },
	{ "still",       still,         0, TRUE },

	{ "tl_start",    tl_start,   1, TRUE },
	{ "tl_end",      tl_end,     0, TRUE },
	{ "tl_hold",    tl_hold,     1, TRUE },
	{ "tl_show_status",  tl_show_status,   1, FALSE },

	{ "motion",        motion_cmd,     1, FALSE },
	{ "motion_enable", motion_enable,  1, TRUE },

	/* Above commands are redirected to abort a menu or adjustment display
	*/
	{ "display",       display_cmd,     1, FALSE },

	/* Below commands are not redirected to abort a menu or adjustment */
	{ "tl_inform_convert",    tl_inform_convert,   1, FALSE },

	{ "still_quality", still_quality,  1, TRUE },
	{ "video_bitrate", video_bitrate,  1, TRUE },
	{ "video_fps", video_fps,  1, TRUE },
	{ "video_mp4box_fps", video_mp4box_fps,  1, TRUE },
	{ "inform", inform,    1, FALSE },
	{ "save_config", save_config,    0, TRUE },
	{ "archive_video", archive_video,    1, TRUE },
	{ "archive_still", archive_still,    1, TRUE },
	{ "delete_log", delete_log,    0, TRUE },
	{ "fix_thumbs", fix_thumbs,    1, TRUE },
	{ "annotate_string", annotate_string, 1, FALSE },
	{ "preset", preset_cmd, 1, FALSE },
	{ "servo", servo_cmd, 1, FALSE },
	{ "multicast", multicast,    1, TRUE },
	{ "upgrade", upgrade,    0, TRUE },
	{ "halt", halt,    0, TRUE },
	{ "reboot", reboot,    0, TRUE },
	{ "quit",       quit,    0, TRUE },
	};

#define COMMAND_SIZE	(sizeof(commands) / sizeof(Command))

void
command_process(char *command_line)
	{
	VideoCircularBuffer	*vcb = &video_circular_buffer;
	Command	*cmd;
	char	command[64], args[128], arg1[128], arg2[64], arg3[64], buf[128], *path;
	int		i, n;

	if (!command_line || *command_line == '\0')
		return;

	n = sscanf(command_line, "%63s %127[^\n]", command, args);
	if (n < 1 || command[0] == '#')
		return;
	for (cmd = NULL, i = 0; i < COMMAND_SIZE; cmd = NULL, ++i)
		{
		cmd = &commands[i];
		if (!strcmp(command, cmd->name))
			{
			if (cmd->n_args != n - 1)
				{
				log_printf("Wrong number of args for command: %s\n", command);
				return;
				}
			break;
			}
		}
	if (cmd && cmd->do_log)
		log_printf("command_process: %s\n", command_line);
	if (!cmd)
		{
		if (   !config_set_option(command, args, FALSE)
		    && !mmalcam_config_parameter_set(command, args, TRUE)
	       )
			log_printf("Bad command: [%s] [%s]\n", command, args);
		else
			pikrellcam.config_modified = TRUE;
		return;
		}

	if (cmd->code < display_cmd && !display_is_default())
		{
		display_set_default();
		return;
		}

	switch (cmd->code)
		{
		case multicast:
			if (args[0] == ':')
				{	/* msg_id, send "hostname:msg_id args" */
				arg1[0] = '\0';
				sscanf(args, "%63s %127[^\n]", arg2, arg1);
				multicast_send(arg2, arg1);
				}
			else	/* no msg_id, send "hostname args" */
				multicast_send("", args);
			break;

		case record:
			n = sscanf(args, "%127s %63[^\n]", arg1, arg2);
			pthread_mutex_lock(&vcb->mutex);
			if (!strcmp(arg1, "pause"))
				{
				if (vcb->state == VCB_STATE_MANUAL_RECORD)
					vcb->pause = TRUE;
				else
					vcb->pause = FALSE;
				}
			else if (config_boolean_value(arg1) == TRUE)
				{
				if (vcb->pause)
					vcb->pause = FALSE;
				else
					{
					if (vcb->state == VCB_STATE_MOTION_RECORD)
						video_record_stop(vcb);
					vcb->manual_pre_capture = 0;
					vcb->max_record_time = 0;
					if (n == 2)
						sscanf(arg2, "%d %d",
							&vcb->manual_pre_capture, &vcb->max_record_time);
					video_record_start(vcb, VCB_STATE_MANUAL_RECORD_START);
					}
				}
			else
				video_record_stop(vcb);
			pthread_mutex_unlock(&vcb->mutex);
			break;

		case record_pause:
			/* Can pause manual record only.  Because of event_gap/capture
			|  times, I'm not even sure what it would mean to pause a
			|  motion record.
			*/
			pthread_mutex_lock(&vcb->mutex);
			if (vcb->state == VCB_STATE_MANUAL_RECORD)
				vcb->pause = vcb->pause ? FALSE : TRUE;
			else
				vcb->pause = FALSE;
			pthread_mutex_unlock(&vcb->mutex);
			break;

		case still:
			snprintf(buf, sizeof(buf), "%d", pikrellcam.still_sequence);
			path = media_pathname(pikrellcam.still_dir, pikrellcam.still_name_format, 0,
							'N', buf,
							'H', pikrellcam.hostname);

			pikrellcam.still_sequence += 1;
			still_capture(path);
			free(path);
			break;

		case tl_start:
			if ((n = atoi(args)) < 1)
				{
				/* display_inform("string row justify font") */
				display_inform("\"Timelapse period must be > 0\" 3 3 1");
				display_inform("timeout 2");
				break;
				}
			time_lapse.activated = TRUE;
			time_lapse.on_hold = FALSE;
			pikrellcam.state_modified = TRUE;
			snprintf(arg1, sizeof(arg1), "%d", n);

			if (!time_lapse.event)
				{
				time_lapse.sequence = 0;
				++time_lapse.series;
				time_lapse.event = event_add("timelapse",
							pikrellcam.t_now, n, timelapse_capture, NULL);

				display_inform("\"Timelapse Starting\" 3 3 1");
				snprintf(buf, sizeof(buf), "\"Period: %d\" 4 3 1", n);
				display_inform(buf);
				display_inform("timeout 2");
				}
			else			/* Change the period */
				{
				display_inform("\"Timelapse Already Running\" 3 3 1");
				if (n != time_lapse.period)
					{
					snprintf(buf, sizeof(buf),
						"\"Changing period from %d to %d\" 4 3 1",
						time_lapse.period, n);
					display_inform(buf);
					}
				display_inform("timeout 2");
				time_lapse.event->time += (n - time_lapse.period);
				time_lapse.event->period = n;
				}
			time_lapse.period = n;
			config_timelapse_save_status();
			config_set_boolean(&time_lapse.show_status, "on");
			pikrellcam.state_modified = TRUE;
			break;

		case tl_hold:
			if (time_lapse.activated)
				{
				config_set_boolean(&time_lapse.on_hold, args); /* toggle */
				config_timelapse_save_status();
				pikrellcam.state_modified = TRUE;
				if (time_lapse.on_hold)
					display_inform("\"Timelapse on hold\" 3 3 1");
				else
					display_inform("\"Timelapse resuming\" 3 3 1");
				display_inform("timeout 1");
				}
			else
				{
				display_inform("\"Timelapse is not running.\" 3 3 1");
				display_inform("timeout 1");
				time_lapse.on_hold = FALSE;
				}
			break;

		case tl_end:
			if (time_lapse.activated)
				{
				event_remove(time_lapse.event);
				time_lapse.event = NULL;
				time_lapse.activated = FALSE;
				time_lapse.on_hold = FALSE;
				pikrellcam.state_modified = TRUE;
				config_timelapse_save_status();
				exec_no_wait(pikrellcam.timelapse_convert_cmd, NULL);

				config_set_boolean(&time_lapse.show_status, "on");
				pikrellcam.state_modified = TRUE;
				display_inform("\"Timelapse ended.\" 3 3 1");
				display_inform("\"Starting convert...\" 4 3 1");
				display_inform("timeout 2");
				}
			else
				{
				display_inform("\"Timelapse is not running.\" 3 3 1");
				display_inform("timeout 1");
				}
			break;

		case tl_inform_convert:
			if (sscanf(args, "%127s %63[^\n]", arg1, arg2) == 2)
				{
				if (!strcmp(arg1, "done"))
					{
					event_remove(time_lapse.inform_event);
					dup_string(&time_lapse.convert_name, "");
					dup_string(&pikrellcam.timelapse_video_last, pikrellcam.timelapse_video_pending);
					dup_string(&pikrellcam.timelapse_video_pending, "");
					dup_string(&pikrellcam.timelapse_jpeg_last, "");
					pikrellcam.state_modified = TRUE;
					time_lapse.convert_size = 0;
					}
				else
					{
					dup_string(&time_lapse.convert_name, arg2);
					pikrellcam.state_modified = TRUE;
					time_lapse.inform_event = event_add("tl_inform_convert",
							pikrellcam.t_now, 5, timelapse_inform_convert, NULL);
					}
				}
			break;

		case tl_show_status:
			config_set_boolean(&time_lapse.show_status, args);
			pikrellcam.state_modified = TRUE;
			break;

		case display_cmd:
			display_command(args);
			break;

		case motion_cmd:
			motion_command(args);
			break;

		case motion_enable:
			n = motion_frame.motion_enable;
			config_set_boolean(&motion_frame.motion_enable, args);
			pikrellcam.state_modified = TRUE;

			if (n && !motion_frame.motion_enable)
				{
				pthread_mutex_lock(&vcb->mutex);
				if (vcb->state == VCB_STATE_MOTION_RECORD)
					video_record_stop(vcb);
				pthread_mutex_unlock(&vcb->mutex);
				}
			break;

		case video_fps:
			if ((n = atoi(args)) < 1)
				n = 1;
			if (n > 49)
				n = 24;
			camera_adjust_temp.video_fps = n;
			pikrellcam.camera_adjust.video_fps = n;
			camera_restart();
			pikrellcam.config_modified = TRUE;
			break;

		case video_bitrate:
			if ((n = atoi(args)) < 1000000)
				n = 1000000;
			if (n > 25000000)
				n = 25000000;
			camera_adjust_temp.video_bitrate = n;
			pikrellcam.camera_adjust.video_bitrate = n;
			camera_restart();
			pikrellcam.config_modified = TRUE;
			break;

		case still_quality:
			if ((n = atoi(args)) < 5)
				n = 5;
			if (n > 100)
				n = 100;
			camera_adjust_temp.still_quality = n;
			pikrellcam.camera_adjust.still_quality = n;
			camera_restart();
			pikrellcam.config_modified = TRUE;
			break;

		case video_mp4box_fps:
			n = atoi(args);
			camera_adjust_temp.video_mp4box_fps = n;
			pikrellcam.camera_adjust.video_mp4box_fps = n;
			pikrellcam.config_modified = TRUE;
			break;

		case inform:
			display_inform(args);
			break;

		case save_config:
			config_save(pikrellcam.config_file);
			break;

		case archive_video:		/* ["day"|video.mp4] yyyy-mm-dd */
			if (sscanf(args, "%127s %63s", arg1, arg2) == 2)
				{
				snprintf(buf, sizeof(buf),
						"%s/scripts-dist/_archive-video %s %s $a $m $P $G",
						pikrellcam.install_dir, arg1, arg2);
				exec_no_wait(buf, NULL);
				}
			else
				log_printf("Wrong number of args for command: %s\n", command);
			break;

		case archive_still:		/* ["day"|still.jpg] yyyy-mm-dd */
			if (sscanf(args, "%127s %63s", arg1, arg2) == 2)
				{
				snprintf(buf, sizeof(buf),
						"%s/scripts-dist/_archive-still %s %s $a $m $P $G",
						pikrellcam.install_dir, arg1, arg2);
				exec_no_wait(buf, NULL);
				}
			else
				log_printf("Wrong number of args for command: %s\n", command);
			break;

		case delete_log:
			unlink(pikrellcam.log_file);
			break;

		case fix_thumbs:
			if (strcmp(args, "fix") && strcmp(args, "test"))
				log_printf("%s: bad arg.  Argument must be \"fix\" or \"test\".\n", command);
			else
				{
				snprintf(buf, sizeof(buf),
							"%s/scripts-dist/_fix_thumbs $m $a $G %s",
							pikrellcam.install_dir, args);
				exec_no_wait(buf, NULL);
				}
			break;

		case annotate_string:		/* annotate_string cmd id text */
			n = sscanf(args, "%127s %63s %63s", arg1, arg2, arg3);
			if (n == 2 && !strcmp(arg1, "remove"))
				annotate_string_remove(arg2);
			else if (n == 2 && !strcmp(arg1, "spacechar"))
				pikrellcam.annotate_string_space_char = arg2[0];
			else if (n == 3)
				annotate_string_add(arg1, arg2, arg3);
			else
				log_printf("Wrong number of args for command: %s\n", command);
			break;

		case preset_cmd:
			preset_command(args);
			break;

		case servo_cmd:
			servo_command(args);
			break;

		case upgrade:
			snprintf(buf, sizeof(buf), "%s/scripts-dist/_upgrade $I $P $G $Z",
						pikrellcam.install_dir);
			exec_no_wait(buf, NULL);
			break;

		case halt:
			if (pikrellcam.halt_enable)
				{
				pikrellcam_cleanup();
				event_shutdown_request(0);
				}
			else
				{
				display_inform("\"Cannot halt the system.\" 3 3 1");
				display_inform("\"Set halt_enable in pikrellcam.conf first.\" 4 3 1");
				display_inform("timeout 3");
				}
			break;

		case reboot:
			if (pikrellcam.halt_enable)
				{
				pikrellcam_cleanup();
				event_shutdown_request(1);
				}
			else
				{
				display_inform("\"Cannot reboot the system.\" 3 3 1");
				display_inform("\"Set halt_enable in pikrellcam.conf first.\" 4 3 1");
				display_inform("timeout 3");
				}
			break;

		case quit:
			pikrellcam_cleanup();
			display_quit();
			exit(0);
			break;

		default:
			log_printf_no_timestamp("command in table with no action!\n");
			break;
		}
	}


static void
check_modes(char *fname, int mode)
	{
	struct stat		st;
	struct group	*grp;
	struct passwd	*pwd;
	char			ch_cmd[200];

	if (stat(fname, &st) == 0)
		{
		grp = getgrgid(st.st_gid);
		pwd = getpwuid(st.st_uid);

		if (   strcmp(pwd->pw_name, pikrellcam.effective_user)
		    || strcmp(grp->gr_name, "www-data")
		   )
			{
			snprintf(ch_cmd, sizeof(ch_cmd), "sudo chown %s.www-data %s",
					pikrellcam.effective_user, fname);
			exec_wait(ch_cmd, NULL);
			}
		if ((st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != mode)
			{
			snprintf(ch_cmd, sizeof(ch_cmd), "sudo chmod %o %s", mode, fname);
			exec_wait(ch_cmd, NULL);
			}
		}
	}


boolean
make_fifo(char *fifo_path)
    {
    boolean		fifo_exists;

	if ((fifo_exists = isfifo(fifo_path)) == FALSE)
		{
        if (mkfifo(fifo_path, 0664) < 0)
		    log_printf_no_timestamp("Make fifo %s failed: %m\n", fifo_path);
		else
			fifo_exists = TRUE;
		}
	if (fifo_exists)
		check_modes(fifo_path, 0664);
	return fifo_exists;
	}

static boolean
make_dir(char *dir)
	{
	boolean 		dir_exists;

	if ((dir_exists = isdir(dir)) == FALSE)
		{
		exec_wait("sudo mkdir -p $F", dir);
		if ((dir_exists = isdir(dir)) == FALSE)
			log_printf_no_timestamp("Make directory failed: %s\n", dir);
		else
			dir_exists = TRUE;
		}
	if (dir_exists)
		check_modes(dir, 0775);
	return dir_exists;
	}

static void
log_start(boolean start_sep, boolean time, boolean end_sep)
	{
	char	buf[100];

	if (start_sep)
		log_printf_no_timestamp("\n========================================================\n");
	if (time)
		{
		strftime(buf, sizeof(buf), "%F %T", localtime(&pikrellcam.t_now));
		log_printf_no_timestamp("======= PiKrellCam %s started at %s\n",
					pikrellcam.version, buf);
		}
	if (end_sep)
		log_printf_no_timestamp("========================================================\n");
	}

int
main(int argc, char *argv[])
	{
	int		fifo;
	int	 	i, j, n;
	char	*opt, *arg, *equal_arg, *user;
	char	*line, *eol, buf[4096];
	int		t_usleep;
	struct timeval	tv;

	pgm_name = argv[0];
	setlocale(LC_TIME, "");

	time(&pikrellcam.t_now);

	for (i = 1; i < argc; i++)
		get_arg_pass1(argv[i]);

	config_set_defaults(homedir);

	if (!config_load(pikrellcam.config_file))
		config_save(pikrellcam.config_file);
	if (quit_flag)	/* Just making sure initial config file is written */
		exit(0);

	if (!pikrellcam.log_file || !*pikrellcam.log_file)
		pikrellcam.log_file = strdup("/dev/null");
	else if (*pikrellcam.log_file != '/')
		{
		snprintf(buf, sizeof(buf), "%s/%s", pikrellcam.install_dir, pikrellcam.log_file);
		dup_string(&pikrellcam.log_file, buf);
		}

	if (*pikrellcam.lc_time)
		{
		log_printf("setting LC_TIME to %s\n", pikrellcam.lc_time);
		setlocale(LC_TIME, pikrellcam.lc_time);
		}

	/* If need to mmap() gpios for servos, restart a sudo pikrellcam which can
	|  mmap() /dev/mem and then drop priviledes back to orig user/group
	*/
	if (getuid() == 0)	/* root, so mmap(), drop privileges and continue */
		{
		log_start(FALSE, TRUE, FALSE);
		servo_init();
		if (user_gid > 0)
			setgid(user_gid);
		setuid(user_uid);
		log_printf_no_timestamp("== Dropped root priviledges-continuing as normal user ==\n");
		log_start(FALSE, FALSE, TRUE);
		}
	else if (pikrellcam.have_servos && !pikrellcam.servo_use_servoblaster)
		{
		/* Need to restart pikrellcam as root so can mmap() PWMs for servos.
		*/
		log_start(TRUE, FALSE, FALSE);
		log_printf_no_timestamp("========= Restarting as root to mmap() servos ==========\n");
		homedir = getpwuid(geteuid())->pw_dir;
		i = snprintf(buf, sizeof(buf), "sudo -P %s -user%d -group%d -home%s ",
				*argv++, (int) getuid(), (int) getgid(), homedir);
		while (--argc && i < sizeof(buf) - 64 - strlen(*argv))
			i += sprintf(buf + i, "%s ", *argv++);

		set_exec_with_session(FALSE);
		exec_wait(buf, NULL);	/*  restart as root so can mmap() gpios*/
		exit(0);
		}
	else if (pikrellcam.servo_use_servoblaster)
		{
		log_start(TRUE, TRUE,FALSE);
		servo_init();
		log_start(FALSE, FALSE, TRUE);
		}
        else if (pikrellcam.servo_use_servohat)
                {
		log_start(TRUE, TRUE,FALSE);
		servo_init();
		log_start(FALSE, FALSE, TRUE);
                }
	else
		log_start(TRUE, TRUE, TRUE);

	bcm_host_init();
	if (!homedir)
		homedir = getpwuid(geteuid())->pw_dir;

	user = strrchr(homedir, '/');
	pikrellcam.effective_user = strdup(user ? user + 1 : "pi");

	if (!motion_regions_config_load(pikrellcam.motion_regions_config_file, FALSE))
		motion_regions_config_save(pikrellcam.motion_regions_config_file, FALSE);
	preset_config_load();

	if (!at_commands_config_load(pikrellcam.at_commands_config_file))
		at_commands_config_save(pikrellcam.at_commands_config_file);

	for (i = 1; i < argc; i++)
		{
		if (get_arg_pass1(argv[i]))
			continue;
		opt = argv[i];

		/* Accept: --opt arg   -opt arg    opt=arg    --opt=arg    -opt=arg
		*/
		for (j = 0; j < 2; ++j)
			if (*opt == '-')
				++opt;
		if ((equal_arg = strchr(opt, '=')) != NULL)
			{
			*equal_arg++ = '\0';
			arg = equal_arg;
			++i;
			}
		else
			{
			arg = argv[i + 1];
			++i;
			}

		/* For camera parameters, do not set the camera, only replace
		|  values in the parameter table.
		*/
		if (   !config_set_option(opt, arg, TRUE)
		    && !mmalcam_config_parameter_set(opt, arg, FALSE)
		   )
			{
			log_printf_no_timestamp("Bad arg: %s\n", opt);
			exit(1);
			}
		else
			pikrellcam.config_modified = TRUE;
		}

	if (pikrellcam.debug)
		printf("debugging...\n");

	if (*pikrellcam.media_dir != '/')
		{
		snprintf(buf, sizeof(buf), "%s/%s", pikrellcam.install_dir, pikrellcam.media_dir);
		dup_string(&pikrellcam.media_dir, buf);
		}
	if (*pikrellcam.archive_dir != '/')
		{
		snprintf(buf, sizeof(buf), "%s/%s", pikrellcam.media_dir, pikrellcam.archive_dir);
		dup_string(&pikrellcam.archive_dir, buf);
		}

	snprintf(buf, sizeof(buf), "%s/%s", pikrellcam.install_dir, "www");
	check_modes(buf, 0775);

	asprintf(&pikrellcam.command_fifo, "%s/www/FIFO", pikrellcam.install_dir);
	asprintf(&pikrellcam.scripts_dir, "%s/scripts", pikrellcam.install_dir);
	asprintf(&pikrellcam.scripts_dist_dir, "%s/scripts-dist", pikrellcam.install_dir);
	asprintf(&pikrellcam.mjpeg_filename, "%s/mjpeg.jpg", pikrellcam.tmpfs_dir);
	asprintf(&pikrellcam.state_filename, "%s/state", pikrellcam.tmpfs_dir);

	log_printf_no_timestamp("using FIFO: %s\n", pikrellcam.command_fifo);
	log_printf_no_timestamp("using mjpeg: %s\n", pikrellcam.mjpeg_filename);


	/* Subdirs must match www/config.php and the init script is supposed
	|  to take care of that.
	*/
	asprintf(&pikrellcam.video_dir, "%s/%s", pikrellcam.media_dir, PIKRELLCAM_VIDEO_SUBDIR);
	asprintf(&pikrellcam.thumb_dir, "%s/%s", pikrellcam.media_dir, PIKRELLCAM_THUMBS_SUBDIR);
	asprintf(&pikrellcam.still_dir, "%s/%s", pikrellcam.media_dir, PIKRELLCAM_STILL_SUBDIR);
	asprintf(&pikrellcam.timelapse_dir, "%s/%s", pikrellcam.media_dir, PIKRELLCAM_TIMELAPSE_SUBDIR);

	if (   !make_dir(pikrellcam.media_dir)		// after startup script, will make
		|| !make_dir(pikrellcam.archive_dir)	// dirs again in case of mount
	   )
		exit(1);

	snprintf(buf, sizeof(buf), "%s/scripts-dist/_init $I $a $m $M $P $G %s",
		pikrellcam.install_dir,
		(pikrellcam.have_servos) ? "servos_on" : "servos_off");
	exec_wait(buf, NULL);

	/* User may have enabled a mount disk on media_dir
	*/
	exec_wait(pikrellcam.on_startup_cmd, NULL);
	check_modes(pikrellcam.media_dir, 0775);
	check_modes(pikrellcam.archive_dir, 0775);
	check_modes(pikrellcam.log_file, 0664);

	if (   !make_dir(pikrellcam.tmpfs_dir)
	    || !make_dir(pikrellcam.archive_dir)
	    || !make_dir(pikrellcam.video_dir)
	    || !make_dir(pikrellcam.thumb_dir)
	    || !make_dir(pikrellcam.still_dir)
	    || !make_dir(pikrellcam.timelapse_dir)
	    || !make_fifo(pikrellcam.command_fifo)
	   )
		exit(1);

	if ((fifo = open(pikrellcam.command_fifo, O_RDONLY | O_NONBLOCK)) < 0)
		{
		log_printf("Failed to open FIFO: %s.  %m\n", pikrellcam.command_fifo);
		exit(1);
		}

	fcntl(fifo, F_SETFL, 0);
	read(fifo, buf, sizeof(buf));
	
	camera_start();
	config_timelapse_load_status();
	preset_state_load();
	pikrellcam.state_modified = TRUE;

	signal(SIGINT, signal_quit);
	signal(SIGTERM, signal_quit);
	signal(SIGCHLD, event_child_signal);

	setup_h264_tcp_server();
	setup_mjpeg_tcp_server();
	multicast_init();

	while (1)
		{
		gettimeofday(&tv, NULL);
		t_usleep = (int) (100000 - (tv.tv_usec % 100000));  /* EVENT_LOOP_FREQUENCY!! */
		usleep(t_usleep + 1);
		time(&pikrellcam.t_now);

		event_process();
		multicast_recv();
		tcp_poll_connect();

		/* Process lines in the FIFO.  Single lines via an echo "xxx" > FIFO
		|  or from a web page may not have a terminating \n.
		|  Local scripts may dump multiple \n terminated lines into the FIFO.
		*/
		if ((n = read(fifo, buf, sizeof(buf) - 2)) > 0)
			{
			if (buf[n - 1] != '\n')
				buf[n++] = '\n';	/* ensures all lines in buf end in \n */
			buf[n] = '\0';
			line = buf;
			eol = strchr(line, '\n');

			while (eol > line)
				{
				*eol++ = '\0';
				command_process(line);
				while (*eol == '\n')
					++eol;
				line = eol;
				eol = strchr(line, '\n');
				}
			}
		}

	//close listening socket
	close (listenfd);  

	return 0;
	}
