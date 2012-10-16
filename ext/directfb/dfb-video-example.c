
#include <gst/gst.h>
#include <libgen.h>
#include <directfb.h>

static IDirectFB *dfb = NULL;
static IDirectFBSurface *primary = NULL;
static GstElement *pipeline, *videocrop;
static struct timeval prev_tv;
static gdouble playback_rate = 1.0;
static gint64 position;
static gboolean is_quick_seek = FALSE;

#define COMMAND_BUF_SIZE 32
#define PARAM_BUF_SIZE 32

#define DFBCHECK(x...)                                         \
  {                                                            \
    DFBResult err = x;                                         \
                                                               \
    if (err != DFB_OK)                                         \
      {                                                        \
        fprintf( stderr, "%s <%d>:\n\t", __FILE__, __LINE__ ); \
        DirectFBErrorFatal( #x, err );                         \
      }                                                        \
  }

static void
usage (char *cmd)
{
  printf ("Usage: %s [OPTION...]\n", basename (cmd));
  printf ("  -x		x of sub surface rectangle\n");
  printf ("  -y		y of sub surface rectangle\n");
  printf ("  -w		w of sub surface rectangle\n");
  printf ("  -h		h of sub surface rectangle\n");
  printf ("  -l		specify the number of display layer\n");
  printf ("  -o		DirectFB or GStreamer option\n");
  printf ("  -i		ignore image's aspect ratio\n");
  printf ("  -f		specify input filename\n");
  printf ("  -q		do quick seeking without accurate positioning\n");
  printf ("  -T		top of cropped image\n");
  printf ("  -B		bottom of cropped image\n");
  printf ("  -L		left of cropped image\n");
  printf ("  -R		right of cropped image\n");
}

static void
create_video_pipeline (GstPad * pad, gpointer data)
{
  GstPad *sinkpad;
  GstElement *peer_element = (GstElement *) data;
  GstCaps *caps;
  GstStructure *structure;
  const gchar *mime;
  static GstElement *queue;
  static GstElement *decoder, *parser;

  if (queue) {
    /* If queue plugin has been already created, queue plugin just gets
       a link to a peer pad. This route is passed when the state is changed. */
    sinkpad = gst_element_get_static_pad (queue, "sink");
    gst_pad_link (pad, sinkpad);
    return;
  }

  caps = GST_PAD_CAPS (pad);
  structure = gst_caps_get_structure (caps, 0);

  mime = gst_structure_get_name (structure);
  if (strcmp (mime, "video/x-h264") == 0) {     /* h.264 */
    printf ("codec type video/x-h264\n");
    parser = gst_element_factory_make ("legacyh264parse", NULL);
    g_assert (parser);
    decoder = gst_element_factory_make ("omx_h264dec", NULL);
    g_assert (decoder);
  } else if (strcmp (mime, "video/mpeg") == 0) {        /* mpeg4 */
    gint ver = 0;

    gst_structure_get_int (structure, "mpegversion", &ver);
    if (ver == 4) {
      printf ("codec type video/mpeg mpegversion=4\n");
      decoder = gst_element_factory_make ("omx_mpeg4dec", NULL);
      g_assert (decoder);
    } else {
      printf ("unsupported format\n");
      return;
    }
  } else if (strcmp (mime, "video/x-wmv") == 0) {       /* vc-1 */
    printf ("codec type video/x-wmv\n");
    decoder = gst_element_factory_make ("omx_wmvdec", NULL);
    g_assert (decoder);
  } else {
    printf ("%s isn't supported.\n", mime);
    return;
  }

  queue = gst_element_factory_make ("queue", NULL);
  g_assert (queue);

  if (parser) {
    g_object_set (parser, "output-format", 1, "split-packetized", TRUE, NULL);
    gst_bin_add_many (GST_BIN (pipeline), parser, decoder, queue, NULL);
    sinkpad = gst_element_get_static_pad (queue, "sink");
    gst_pad_link (pad, sinkpad);
    if (videocrop)
      gst_element_link_many (queue, parser, decoder, videocrop, peer_element,
          NULL);
    else
      gst_element_link_many (queue, parser, decoder, peer_element, NULL);
    gst_object_unref (sinkpad);

    gst_element_set_state (queue, GST_STATE_PLAYING);
    gst_element_set_state (parser, GST_STATE_PLAYING);
    gst_element_set_state (decoder, GST_STATE_PLAYING);
  } else {
    gst_bin_add_many (GST_BIN (pipeline), decoder, queue, NULL);
    sinkpad = gst_element_get_static_pad (queue, "sink");
    gst_pad_link (pad, sinkpad);
    if (videocrop)
      gst_element_link_many (queue, decoder, videocrop, peer_element, NULL);
    else
      gst_element_link_many (queue, decoder, peer_element, NULL);
    gst_object_unref (sinkpad);

    gst_element_set_state (queue, GST_STATE_PLAYING);
    gst_element_set_state (decoder, GST_STATE_PLAYING);
  }
}

static void
create_audio_pipeline (GstPad * pad)
{
  GstPad *sinkpad;
  GstCaps *caps;
  GstStructure *structure;
  const gchar *mime;
  GstElement *decoder, *sink;
  static GstElement *queue;

  if (queue) {
    /* If queue plugin has been already created, demuxer plugin just gets
       a link to a peer pad. This route is passed when the state is changed. */
    sinkpad = gst_element_get_static_pad (queue, "sink");
    gst_pad_link (pad, sinkpad);
    gst_object_unref (sinkpad);
    return;
  }

  caps = GST_PAD_CAPS (pad);
  structure = gst_caps_get_structure (caps, 0);

  mime = gst_structure_get_name (structure);
  if (strcmp (mime, "audio/mpeg") == 0) {       /* AAC or MP3 */
    gint ver = 0, layer = 0;

    gst_structure_get_int (structure, "mpegversion", &ver);
    gst_structure_get_int (structure, "layer", &layer);
    /* This test to determine is based on "http://gstreamer.freedesktop.org/data/doc/gstreamer/head/pwg/html/section-types-definitions.html". */
    if (ver == 4 || ver == 2) { /* AAC */
      printf ("codec type video/mpeg mpegversion=%d\n", ver);
      decoder = gst_element_factory_make ("faad", NULL);
      if (!decoder) {
        printf ("faad plugin wasn't found\n");
        return;
      }
    } else if (ver == 1 && layer == 3) {        /* MP3 */
      printf ("codec type video/mpeg mpegversion=1\n");
      decoder = gst_element_factory_make ("mad", NULL);
      if (!decoder) {
        printf ("mad plugin wasn't found\n");
        return;
      }
    } else {
      printf ("unsupported format\n");
      return;
    }
  } else {
    printf ("%s isn't supported.\n", mime);
    return;
  }

  queue = gst_element_factory_make ("queue", NULL);
  g_assert (queue);
  sink = gst_element_factory_make ("alsasink", NULL);
  g_assert (sink);

  gst_bin_add_many (GST_BIN (pipeline), queue, decoder, sink, NULL);
  sinkpad = gst_element_get_static_pad (queue, "sink");
  gst_pad_link (pad, sinkpad);
  gst_element_link_many (queue, decoder, sink, NULL);
  gst_object_unref (sinkpad);

  gst_element_set_state (queue, GST_STATE_PLAYING);
  gst_element_set_state (decoder, GST_STATE_PLAYING);
  gst_element_set_state (sink, GST_STATE_PLAYING);
}

static void
on_pad_added (GstElement * element, GstPad * pad, gpointer data)
{
  /* We can now link this pad with the gst-omx decoder or h264parse sink pad */
  printf ("Dynamic pad created, linking\n");

  if (strcmp (gst_pad_get_name (pad), "video_00") == 0)
    create_video_pipeline (pad, data);
  else if (strcmp (gst_pad_get_name (pad), "audio_00") == 0)
    create_audio_pipeline (pad);
  else
    printf ("%s isn't acceptable.\n", gst_pad_get_name (pad));
}

static void
play_handler (int signum)
{
  switch (signum) {
    case SIGQUIT:
    {
      static int n = 0;

      if (n) {
        printf ("set state = GST_STATE_PLAYING\n");
        gst_element_set_state (pipeline, GST_STATE_PLAYING);
      } else {
        printf ("set state = GST_STATE_PAUSED\n");
        gst_element_set_state (pipeline, GST_STATE_PAUSED);
      }
      n = !n;
    }
      break;
    case SIGINT:
      gst_element_post_message (GST_ELEMENT (pipeline),
          gst_message_new_application (GST_OBJECT (pipeline),
              gst_structure_new ("GstVideExampleInterrupt", "message",
                  G_TYPE_STRING, "Pipeline interrupted", NULL)));
      break;
    default:
      break;
  }
}

static void
display_help (void)
{
  printf (" 0 --- Stop movie\n");
  printf (" 1 --- Ready movie (not used)\n");
  printf (" 2 --- Pause movie\n");
  printf (" 3 --- Playing movie\n");
  printf (" seek [number(sec)] --- seek to specified time later\n");
  printf (" rate [playback rate] --- playback rate\n");
  printf (" h --- Help\n");
}

static gboolean
channel_cb (GIOChannel * source, GIOCondition condition, gpointer data)
{
  int rc;
  char command[COMMAND_BUF_SIZE], param[PARAM_BUF_SIZE];
  gint64 cur_time;

  if (condition != G_IO_IN)
    return TRUE;

  rc = fscanf (stdin, "%s %s", command, param);
  if (rc <= 0) {
    printf ("The value EOF is returned.\n");
    return TRUE;
  }

  if (strcmp (command, "seek") == 0) {
    GstFormat fmt = GST_FORMAT_TIME;
    GstSeekFlags seek_flags;

    if (is_quick_seek)
      seek_flags = GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH;
    else
      seek_flags = GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_FLUSH;

    printf ("perform seeking\n");
    if (!gst_element_query_position (pipeline, &fmt, &cur_time)) {
      printf ("failed to get current time\n");
      return TRUE;
    }

    cur_time += atoi (param) * GST_SECOND;

    if (!gst_element_seek (pipeline, MIN (1.0, playback_rate),
            GST_FORMAT_TIME,
            seek_flags,
            GST_SEEK_TYPE_SET, cur_time,
            GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)) {
      printf ("failed to seek");
      return TRUE;
    }
  } else if (strcmp (command, "rate") == 0) {
    GstFormat fmt = GST_FORMAT_TIME;
    gdouble rate;
    GstSeekFlags seek_flags;

    if (is_quick_seek)
      seek_flags = GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH;
    else
      seek_flags = GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_FLUSH;

    if (!gst_element_query_position (pipeline, &fmt, &position)) {
      printf ("failed to get current time\n");
      return TRUE;
    }

    rate = atof (param);

    if (rate > 1.0 || rate < -1.0) {
      printf ("change playback rate to %0.5lf\n", rate);
      gettimeofday (&prev_tv, NULL);
      playback_rate = rate;
      if (!gst_element_seek (pipeline, 1.0, GST_FORMAT_TIME,
              seek_flags,
              GST_SEEK_TYPE_SET, position,
              GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)) {
        printf ("failed to seek\n");
        return TRUE;
      }
    } else if (rate > 0.0 && rate <= 1.0) {
      printf ("change playback rate to %0.5lf\n", rate);
      playback_rate = rate;
      if (!gst_element_seek (pipeline, rate, GST_FORMAT_TIME,
              seek_flags,
              GST_SEEK_TYPE_SET, position,
              GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)) {
        printf ("failed to seek\n");
        return TRUE;
      }
    } else {
      printf ("unsupported playback rate\n");
    }
  } else if (strcmp (command, "0") == 0) {
    /* Movie STOP */
    gst_element_set_state (pipeline, GST_STATE_NULL);
    printf ("set state = GST_STATE_NULL\n");
  } else if (strcmp (command, "1") == 0) {
    /* Movie READY (maybe not use) */
    gst_element_set_state (pipeline, GST_STATE_READY);
    printf ("set state = GST_STATE_READY\n");
  } else if (strcmp (command, "2") == 0) {
    /* Movie PAUSE */
    gst_element_set_state (pipeline, GST_STATE_PAUSED);
    printf ("set state = GST_STATE_PAUSED\n");
  } else if (strcmp (command, "3") == 0) {
    /* Movie PLAYING */
    gst_element_set_state (pipeline, GST_STATE_PLAYING);
    printf ("set state = GST_STATE_PLAYING\n");
  } else if (strcmp (command, "h") == 0 || strcmp (command, "H") == 0) {
    /* display help */
    display_help ();
  }

  return TRUE;
}

static void
event_loop (GstElement * pipeline)
{
  GstBus *bus;
  GstMessage *message = NULL;
  gboolean loop = TRUE;

  bus = gst_element_get_bus (GST_ELEMENT (pipeline));

  while (loop) {
    message = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (message)) {
      case GST_MESSAGE_EOS:
        loop = FALSE;
        break;
      case GST_MESSAGE_ERROR:
        printf ("an error in gstreamer was occured.\n");
        loop = FALSE;
        break;
      case GST_MESSAGE_APPLICATION:
      {
        const GstStructure *s;

        s = gst_message_get_structure (message);

        if (gst_structure_has_name (s, "GstVideExampleInterrupt"))
          loop = FALSE;
      }
        break;
      case GST_MESSAGE_ELEMENT:
      {
        const GstStructure *s;
        gint64 duration;
        struct timeval cur_tv;

        s = gst_message_get_structure (message);

        if (gst_structure_has_name (s, "FrameRendered")) {
          GstFormat fmt = GST_FORMAT_TIME;
          gint64 total_duration;
          GstSeekFlags seek_flags;

          if (playback_rate > 0.0 && playback_rate <= 1.0)
            break;

          if (is_quick_seek)
            seek_flags = GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH;
          else
            seek_flags = GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_FLUSH;

          gettimeofday (&cur_tv, NULL);
          duration = (((gint64) cur_tv.tv_sec * 1000000 + cur_tv.tv_usec) - ((gint64) prev_tv.tv_sec * 1000000 + prev_tv.tv_usec)) * 1000;      /* calculation in nano second */
          memcpy (&prev_tv, &cur_tv, sizeof (prev_tv));

          position += playback_rate * duration;
          gst_element_query_duration (pipeline, &fmt, &total_duration);

          if (position < 0 || position > total_duration) {
            /* finish when going beyond playback time */
            loop = FALSE;
            break;
          }

          if (!gst_element_seek (pipeline, 1.0, GST_FORMAT_TIME,
                  seek_flags,
                  GST_SEEK_TYPE_SET, position,
                  GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)) {
            printf ("failed to seek\n");
            return;
          }
        }
      }
        break;
      default:
        break;
    }
  }
}

int
main (int argc, char *argv[])
{
  GstElement *src, *demuxer, *sink;
  int screen_width, screen_height;
  IDirectFBDisplayLayer *layer;
  DFBDisplayLayerID layer_id;
  DFBDisplayLayerConfig config;
  DFBRectangle rect;
  int opt;
  int tmp_argc;
  char **tmp_argv;
  int i;
  gboolean is_keep_aspect = TRUE;
  char *in_file = NULL, *ext;
  struct sigaction action;
  GIOChannel *chan;
  int top, bottom, left, right;

  if ((argc < 3) || (strcmp (argv[1], "--help") == 0)) {
    usage (argv[0]);
    exit (1);
  }

  tmp_argc = 3;
  tmp_argv = (char **) malloc (sizeof (char *) * MAX (argc, tmp_argc));
  tmp_argv[0] = argv[0];
  tmp_argv[1] = strdup ("--dfb:quiet");
  tmp_argv[2] = strdup ("--dfb:no-sighandler");

  memset (&rect, 0, sizeof (rect));
  layer_id = 0;
  top = bottom = left = right = 0;

  opterr = 0;
  while ((opt = getopt (argc, argv, "x:y:w:h:l:o:if:qT:B:L:R:")) != -1) {
    switch (opt) {
      case 'x':
        rect.x = atoi (optarg);
        break;
      case 'y':
        rect.y = atoi (optarg);
        break;
      case 'w':
        rect.w = atoi (optarg);
        break;
      case 'h':
        rect.h = atoi (optarg);
        break;
      case 'l':
        layer_id = atoi (optarg);
        break;
      case 'o':
        tmp_argv[tmp_argc++] = strdup (optarg);
        break;
      case 'i':
        is_keep_aspect = FALSE;
        break;
      case 'f':
        in_file = strdup (optarg);
        break;
      case 'q':
        is_quick_seek = TRUE;
        break;
      case 'T':
        top = atoi (optarg);
        break;
      case 'B':
        bottom = atoi (optarg);
        break;
      case 'L':
        left = atoi (optarg);
        break;
      case 'R':
        right = atoi (optarg);
        break;
      default:
        usage (argv[0]);
        exit (1);
        break;
    }
  }

  /* Init both GStreamer and DirectFB */
  DFBCHECK (DirectFBInit (&tmp_argc, &tmp_argv));
  gst_init (&tmp_argc, &tmp_argv);

  /* Creates DirectFB main context and set it to fullscreen layout */
  DFBCHECK (DirectFBCreate (&dfb));
  DFBCHECK (dfb->GetDisplayLayer (dfb, layer_id, &layer));
  DFBCHECK (layer->SetCooperativeLevel (layer, DLSCL_EXCLUSIVE));

  /* We want a double buffered primary surface */
  config.flags = DLCONF_BUFFERMODE | DLCONF_SURFACE_CAPS;
  config.buffermode = DLBM_BACKVIDEO;
  config.surface_caps = DSCAPS_FLIPPING;

  DFBCHECK (layer->SetConfiguration (layer, &config));
  DFBCHECK (layer->GetSurface (layer, &primary));
  DFBCHECK (primary->GetSize (primary, &screen_width, &screen_height));

  /* default setting */
  if (rect.w == 0)
    rect.w = screen_width;

  if (rect.h == 0)
    rect.h = screen_height;

  /* Creating our pipeline : v4l2src ! dfbvideosink (optional: videocrop) */
  pipeline = gst_pipeline_new (NULL);
  g_assert (pipeline);

  src = gst_element_factory_make ("filesrc", NULL);
  g_assert (src);

  /* parse file extension */
  ext = strchr (in_file, '.');
  if (strcasecmp (ext, ".mp4") == 0 ||
      strcasecmp (ext, ".m4v") == 0 ||
      strcasecmp (ext, ".mov") == 0 || strcasecmp (ext, ".3gp") == 0)
    demuxer = gst_element_factory_make ("qtdemux", NULL);
  else if (strcasecmp (ext, ".avi") == 0)
    demuxer = gst_element_factory_make ("avidemux", NULL);
  else if (strcasecmp (ext, ".wmv") == 0 || strcasecmp (ext, ".asf") == 0)
    demuxer = gst_element_factory_make ("asfdemux", NULL);
  else {
    printf ("Can't recognize filename extension.\n");
    exit (1);
  }
  g_assert (demuxer);

  sink = gst_element_factory_make ("dfbvideosink", NULL);
  g_assert (sink);

  /* set filename to filesrc */
  g_object_set (src, "location", in_file, NULL);

  /* That's the interesting part, giving the primary surface to dfbvideosink.
     And keep-aspect-ratio is set. */
  g_object_set (sink, "surface", primary, "keep-aspect-ratio",
      is_keep_aspect, "window-width", rect.w, "window-height", rect.h,
      "window-x", rect.x, "window-y", rect.y, NULL);

  if (top || bottom || left || right) {
    videocrop = gst_element_factory_make ("videocrop", NULL);
    g_assert (videocrop);

    g_object_set (videocrop, "top", top, "bottom", bottom, "left", left,
        "right", right, NULL);
  } else
    videocrop = NULL;

  if (videocrop)
    gst_bin_add_many (GST_BIN (pipeline), src, demuxer, sink, videocrop, NULL);
  else
    gst_bin_add_many (GST_BIN (pipeline), src, demuxer, sink, NULL);
  if (!gst_element_link_many (src, demuxer, NULL))
    g_error ("Couldn't link src, crop, and sink");

  g_signal_connect (demuxer, "pad-added", G_CALLBACK (on_pad_added), sink);

  primary->Clear (primary, 0x00, 0x00, 0x00, 0xFF);
  primary->Flip (primary, NULL, DSFLIP_NONE);
  primary->Clear (primary, 0x00, 0x00, 0x00, 0xFF);

  /* set signal handler */
  action.sa_handler = play_handler;
  sigaction (SIGQUIT, &action, NULL);
  sigaction (SIGINT, &action, NULL);

  chan = g_io_channel_unix_new (0);
  g_io_add_watch (chan, G_IO_IN, channel_cb, NULL);
  g_io_channel_set_flags (chan, G_IO_FLAG_NONBLOCK, NULL);
  g_io_channel_set_close_on_unref (chan, TRUE);
  g_io_channel_set_encoding (chan, NULL, NULL);

  /* Let's play ! */
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* we need to run a GLib main loop to get out of here */
  event_loop (pipeline);

  /* Release elements and stop playback */
  gst_element_set_state (pipeline, GST_STATE_NULL);

  /* Release DirectFB context and surface */
  primary->Release (primary);
  layer->Release (layer);
  dfb->Release (dfb);

  gst_deinit ();

  if (in_file)
    free (in_file);

  for (i = 1; i < tmp_argc; i++) {
    free (tmp_argv[i]);
  }
  free (tmp_argv);

  return 0;
}
