
#include <stdio.h>
#include <gst/gst.h>
#include <libgen.h>
#include <directfb.h>

static IDirectFB *dfb = NULL;
static IDirectFBSurface *primary = NULL;
static GMainLoop *loop;

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
  printf ("  -u		specify uyvy as v4l2src output pixelformat\n");
  printf ("  -l		specify the number of display layer\n");
  printf
      ("  -q		specify the number of buffers to be enqueud in the v4l2 driver\n");
  printf ("  -T		top of cropped image\n");
  printf ("  -B		bottom of cropped image\n");
  printf ("  -L		left of cropped image\n");
  printf ("  -R		right of cropped image\n");
  printf ("  -o		DirectFB or GStreamer option\n");
  printf ("  -i		ignore image's aspect ratio\n");
}

int
main (int argc, char *argv[])
{
  GstElement *pipeline, *src, *sink, *videocrop;
  int screen_width, screen_height;
  IDirectFBDisplayLayer *layer;
  DFBDisplayLayerID layer_id;
  DFBDisplayLayerConfig config;
  IDirectFBSurface *sub_surface;
  DFBRectangle rect;
  int opt;
  int tmp_argc;
  char **tmp_argv;
  int i;
  GstCaps *caps;
  gboolean is_uyvy = FALSE, is_keep_aspect = TRUE;
  guint32 queue_size;
  int top, bottom, left, right;

  if ((argc == 2) && (strcmp (argv[1], "--help") == 0)) {
    usage (argv[0]);
    exit (1);
  }

  tmp_argc = 2;
  tmp_argv = (char **) malloc (sizeof (char *) * MAX (argc, tmp_argc));
  tmp_argv[0] = argv[0];
  tmp_argv[1] = strdup ("--dfb:quiet");

  memset (&rect, 0, sizeof (rect));
  layer_id = 0;
  queue_size = 5;
  top = bottom = left = right = 0;

  opterr = 0;
  while ((opt = getopt (argc, argv, "x:y:w:h:ul:q:o:T:B:L:R:i")) != -1) {
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
      case 'u':
        is_uyvy = TRUE;
        break;
      case 'l':
        layer_id = atoi (optarg);
        break;
      case 'q':
        queue_size = atoi (optarg);
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
      case 'o':
        tmp_argv[tmp_argc++] = strdup (optarg);
        break;
      case 'i':
        is_keep_aspect = FALSE;
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

  /* get the surface that move to a position specified with a offset
     coordinate based on center */
  primary->GetSubSurface (primary, &rect, &sub_surface);

  /* Creating our pipeline : v4l2src ! dfbvideosink (optional: videocrop) */
  pipeline = gst_pipeline_new (NULL);
  g_assert (pipeline);
  src = gst_element_factory_make ("v4l2src", NULL);
  g_assert (src);
  if (top || bottom || left || right) {
    videocrop = gst_element_factory_make ("videocrop", NULL);
    g_assert (videocrop);
  } else {
    videocrop = NULL;
  }
  sink = gst_element_factory_make ("dfbvideosink", NULL);
  g_assert (sink);

  /* setting zero copy for v4l2src */
  g_object_set (src, "always-copy", FALSE, "queue-size", queue_size, NULL);

  /* That's the interesting part, giving the primary surface to dfbvideosink.
     And keep-aspect-ratio is set. */
  g_object_set (sink, "surface", sub_surface, "keep-aspect-ratio",
      is_keep_aspect, NULL);

  /* Adding elements to the pipeline */
  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);

  if (is_uyvy && videocrop) {
    /* src ! caps (video/x-raw-yuv...) ! videocrop ! sink */
    g_object_set (videocrop, "top", top, "bottom", bottom, "left", left,
        "right", right, NULL);
    gst_bin_add_many (GST_BIN (pipeline), src, videocrop, sink, NULL);
    caps = gst_caps_new_simple ("video/x-raw-yuv",
        "format", GST_TYPE_FOURCC, GST_MAKE_FOURCC ('U', 'Y', 'V', 'Y'), NULL);
    if (!gst_element_link_filtered (src, videocrop, caps))
      g_error ("Couldn't link src, crop, and caps");
    if (!gst_element_link (videocrop, sink))
      g_error ("Couldn't link crop and sink");
    gst_caps_unref (caps);
  } else if (is_uyvy) {
    /* src ! caps (video/x-raw-yuv...) ! sink */
    gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
    caps = gst_caps_new_simple ("video/x-raw-yuv",
        "format", GST_TYPE_FOURCC, GST_MAKE_FOURCC ('U', 'Y', 'V', 'Y'), NULL);
    if (!gst_element_link_filtered (src, sink, caps))
      g_error ("Couldn't link src, caps, and sink");
    gst_caps_unref (caps);
  } else if (videocrop) {
    /* src ! videocrop ! sink */
    g_object_set (videocrop, "top", top, "bottom", bottom, "left", left,
        "right", right, NULL);
    gst_bin_add_many (GST_BIN (pipeline), src, videocrop, sink, NULL);
    if (!gst_element_link_many (src, videocrop, sink, NULL))
      g_error ("Couldn't link src, crop, and sink");
  } else {
    /* src ! sink */
    gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
    if (!gst_element_link (src, sink))
      g_error ("Couldn't link src and sink");
  }

  primary->Clear (primary, 0x00, 0x00, 0x00, 0xFF);
  primary->Flip (primary, NULL, DSFLIP_NONE);
  primary->Clear (primary, 0x00, 0x00, 0x00, 0xFF);

  /* Let's play ! */
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* we need to run a GLib main loop to get out of here */
  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  /* Release elements and stop playback */
  gst_element_set_state (pipeline, GST_STATE_NULL);

  /* Free the main loop */
  g_main_loop_unref (loop);

  /* Release DirectFB context and surface */
  sub_surface->Release (sub_surface);
  primary->Release (primary);
  dfb->Release (dfb);

  for (i = 1; i < tmp_argc; i++) {
    free (tmp_argv[i]);
  }
  free (tmp_argv);

  return 0;
}
