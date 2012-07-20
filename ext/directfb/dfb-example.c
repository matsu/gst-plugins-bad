
#include <directfb.h>
#include <stdio.h>
#include <gst/gst.h>
#include <libgen.h>

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
  printf ("  -o		DirectFB or GStreamer option\n");
}

int
main (int argc, char *argv[])
{
  DFBSurfaceDescription dsc;
  GstElement *pipeline, *src, *sink;
  int screen_width, screen_height;
  IDirectFBSurface *sub_surface;
  DFBRectangle rect;
  int opt;
  int tmp_argc;
  char **tmp_argv;
  int i;

  if ((argc == 2) && (strcmp (argv[1], "--help") == 0)) {
    usage (argv[0]);
    exit (1);
  }

  tmp_argc = 2;
  tmp_argv = (char **) malloc (sizeof (char *) * MAX (argc, tmp_argc));
  tmp_argv[0] = argv[0];
  tmp_argv[1] = strdup ("--dfb:quiet");

  memset (&rect, 0, sizeof (rect));

  opterr = 0;
  while ((opt = getopt (argc, argv, "x:y:w:h:o:")) != -1) {
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
      case 'o':
        tmp_argv[tmp_argc++] = strdup (optarg);
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
  DFBCHECK (dfb->SetCooperativeLevel (dfb, DFSCL_FULLSCREEN));

  /* We want a double buffered primary surface */
  dsc.flags = DSDESC_CAPS;
  dsc.caps = DSCAPS_PRIMARY | DSCAPS_FLIPPING;

  DFBCHECK (dfb->CreateSurface (dfb, &dsc, &primary));
  DFBCHECK (primary->GetSize (primary, &screen_width, &screen_height));

  /* default setting */
  if (rect.w == 0)
    rect.w = screen_width;

  if (rect.h == 0)
    rect.h = screen_height;

  /* get the surface that move to a position specified with a offset
     coordinate based on center */
  primary->GetSubSurface (primary, &rect, &sub_surface);

  /* Creating our pipeline : v4l2src ! dfbvideosink */
  pipeline = gst_pipeline_new (NULL);
  g_assert (pipeline);
  src = gst_element_factory_make ("v4l2src", NULL);
  g_assert (src);
  sink = gst_element_factory_make ("dfbvideosink", NULL);
  g_assert (sink);

  /* setting zero copy for v4l2src */
  g_object_set (src, "always-copy", FALSE, "queue-size", 4, NULL);

  /* That's the interesting part, giving the primary surface to dfbvideosink */
  g_object_set (sink, "surface", sub_surface, NULL);

  /* Adding elements to the pipeline */
  gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);

  if (!gst_element_link (src, sink))
    g_error ("Couldn't link videotestsrc and dfbvideosink");

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
