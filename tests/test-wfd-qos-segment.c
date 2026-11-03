/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Include the implementation to exercise its private QOSData and cleanup,
 * without exporting test-only API or duplicating their layout. Other WFD
 * functions are resolved from the existing static library. */
#include "../src/wfd/wfd-media-factory.c"

static GstPadProbeReturn
observe_flags (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  GstPadProbeType *flags = user_data;

  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) == GST_EVENT_SEGMENT)
    *flags = GST_PAD_PROBE_INFO_TYPE (info);
  return GST_PAD_PROBE_OK;
}

static gboolean
consume_event (GstPad *pad, GstObject *parent, GstEvent *event)
{
  gst_event_unref (event);
  return TRUE;
}

static void
segment_events (void)
{
  g_autoptr(GstPad) src = gst_object_ref_sink (gst_pad_new ("src", GST_PAD_SRC));
  g_autoptr(GstPad) sink = gst_object_ref_sink (gst_pad_new ("sink", GST_PAD_SINK));
  GstPadProbeType flags = GST_PAD_PROBE_TYPE_INVALID;
  QOSData *data = g_new0 (QOSData, 1);

  gst_pad_set_event_function (sink, consume_event);
  g_assert_true (gst_pad_set_active (src, TRUE));
  g_assert_true (gst_pad_set_active (sink, TRUE));
  g_assert_cmpint (gst_pad_link (src, sink), ==, GST_PAD_LINK_OK);
  gulong observer = gst_pad_add_probe (sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                                       observe_flags, &flags, NULL);
  gulong probe = gst_pad_add_probe (sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                                    encoding_perf_probe_cb, data, NULL);
  g_assert_true (gst_pad_push_event (src, gst_event_new_stream_start ("qos-test")));
  g_assert_null (data->segment);

  for (guint i = 1; i <= 3; i++)
    {
      GstSegment segment;
      gst_segment_init (&segment, GST_FORMAT_TIME);
      segment.start = i * 3 * GST_SECOND;
      segment.time = i * 2 * GST_SECOND;
      segment.base = i * GST_SECOND;
      segment.stop = segment.start + 10 * GST_SECOND;
      segment.position = segment.start;
      segment.rate = 0.5;
      segment.applied_rate = 1.25;
      g_autoptr(GstEvent) event = gst_event_new_segment (&segment);
      const GstSegment *event_segment;
      gst_event_parse_segment (event, &event_segment);
      g_assert_true (gst_pad_push_event (src, gst_event_ref (event)));
      g_assert_true ((flags & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0);
      g_assert_true ((flags & GST_PAD_PROBE_TYPE_PUSH) != 0);
      g_assert_nonnull (data->segment);
      g_assert_true (data->segment != event_segment);
      g_assert_true (gst_segment_is_equal (data->segment, &segment));
      g_clear_pointer (&event, gst_event_unref);
      /* The segment must remain owned after the original event is released. */
      g_assert_true (gst_segment_is_equal (data->segment, &segment));

      GstSegment *saved = data->segment;
      g_assert_true (gst_pad_push_event (src, gst_event_new_tag (gst_tag_list_new_empty ())));
      g_assert_true (data->segment == saved);
      g_assert_true (gst_segment_is_equal (data->segment, &segment));
    }

  gst_pad_remove_probe (sink, probe);
  gst_pad_remove_probe (sink, observer);
  g_assert_true (gst_pad_unlink (src, sink));
  gst_pad_set_active (src, FALSE);
  gst_pad_set_active (sink, FALSE);
  free_qos_data (data);
  free_qos_data (g_new0 (QOSData, 1));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);
  g_test_add_func ("/wfd/qos/pushed-segment-events", segment_events);
  return g_test_run ();
}
