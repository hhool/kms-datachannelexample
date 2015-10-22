/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */
#define _XOPEN_SOURCE 500

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "kmsimageoverlaymetadata.h"

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include <glib/gstdio.h>
#include <ftw.h>
#include <string.h>
#include <errno.h>

#include <opencv/cv.h>

#include <opencv/highgui.h>
#include <commons/kmsserializablemeta.h>

#define TEMP_PATH "/tmp/XXXXXX"
#define BLUE_COLOR (cvScalar (255, 0, 0, 0))
#define SRC_OVERLAY ((double)1)

#define PLUGIN_NAME "imageoverlaymetadata"

GST_DEBUG_CATEGORY_STATIC (kms_image_overlay_debug_category);
#define GST_CAT_DEFAULT kms_image_overlay_debug_category

#define KMS_IMAGE_OVERLAY_GET_PRIVATE(obj) ( \
  G_TYPE_INSTANCE_GET_PRIVATE (              \
    (obj),                                   \
    KMS_TYPE_IMAGE_OVERLAY,                  \
    KmsImageOverlayPrivate                   \
  )                                          \
)

enum
{
  PROP_0,
  PROP_SHOW_DEBUG_INFO
};

struct _KmsImageOverlayPrivate
{
  IplImage *cvImage;

  gdouble offsetXPercent, offsetYPercent, widthPercent, heightPercent;
  gboolean show_debug_info;
};

/* pad templates */

#define VIDEO_SRC_CAPS \
    GST_VIDEO_CAPS_MAKE("{ BGR }")

#define VIDEO_SINK_CAPS \
    GST_VIDEO_CAPS_MAKE("{ BGR }")

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsImageOverlay, kms_image_overlay,
    GST_TYPE_VIDEO_FILTER,
    GST_DEBUG_CATEGORY_INIT (kms_image_overlay_debug_category, PLUGIN_NAME,
        0, "debug category for imageoverlay element"));

static void
kms_image_overlay_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  KmsImageOverlay *imageoverlay = KMS_IMAGE_OVERLAY (object);

  GST_OBJECT_LOCK (imageoverlay);

  switch (property_id) {
    case PROP_SHOW_DEBUG_INFO:
      imageoverlay->priv->show_debug_info = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (imageoverlay);
}

static void
kms_image_overlay_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  KmsImageOverlay *imageoverlay = KMS_IMAGE_OVERLAY (object);

  GST_DEBUG_OBJECT (imageoverlay, "get_property");

  GST_OBJECT_LOCK (imageoverlay);

  switch (property_id) {
    case PROP_SHOW_DEBUG_INFO:
      g_value_set_boolean (value, imageoverlay->priv->show_debug_info);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (imageoverlay);
}

static void
kms_image_overlay_display_detections_overlay_img (KmsImageOverlay *
    imageoverlay, const GSList * faces_list)
{
  const GSList *iterator = NULL;

  for (iterator = faces_list; iterator; iterator = iterator->next) {
    CvRect *r = iterator->data;

    cvRectangle (imageoverlay->priv->cvImage, cvPoint (r->x, r->y),
        cvPoint (r->x + r->width, r->y + r->height), cvScalar (0, 255, 0, 0), 3,
        8, 0);
  }
}

static void
kms_image_overlay_initialize_images (KmsImageOverlay * imageoverlay,
    GstVideoFrame * frame)
{
  if (imageoverlay->priv->cvImage == NULL) {
    imageoverlay->priv->cvImage =
        cvCreateImage (cvSize (frame->info.width, frame->info.height),
        IPL_DEPTH_8U, 3);

  } else if ((imageoverlay->priv->cvImage->width != frame->info.width)
      || (imageoverlay->priv->cvImage->height != frame->info.height)) {

    cvReleaseImage (&imageoverlay->priv->cvImage);
    imageoverlay->priv->cvImage =
        cvCreateImage (cvSize (frame->info.width, frame->info.height),
        IPL_DEPTH_8U, 3);
  }
}

static GSList *
get_faces (GstStructure * faces)
{
  gint len, aux;
  GSList *list = NULL;

  len = gst_structure_n_fields (faces);

  for (aux = 0; aux < len; aux++) {
    GstStructure *face;
    gboolean ret;

    const gchar *name = gst_structure_nth_field_name (faces, aux);

    if (g_strcmp0 (name, "timestamp") == 0) {
      continue;
    }

    ret = gst_structure_get (faces, name, GST_TYPE_STRUCTURE, &face, NULL);

    if (ret) {
      CvRect *aux = g_slice_new0 (CvRect);

      gst_structure_get (face, "x", G_TYPE_UINT, &aux->x, NULL);
      gst_structure_get (face, "y", G_TYPE_UINT, &aux->y, NULL);
      gst_structure_get (face, "width", G_TYPE_UINT, &aux->width, NULL);
      gst_structure_get (face, "height", G_TYPE_UINT, &aux->height, NULL);
      gst_structure_free (face);
      list = g_slist_append (list, aux);
    }
  }
  return list;
}

static void
cvrect_free (gpointer data)
{
  g_slice_free (CvRect, data);
}

static GstFlowReturn
kms_image_overlay_transform_frame_ip (GstVideoFilter * filter,
    GstVideoFrame * frame)
{
  KmsImageOverlay *imageoverlay = KMS_IMAGE_OVERLAY (filter);
  GstMapInfo info;
  GSList *faces_list;
  KmsSerializableMeta *metadata;

  gst_buffer_map (frame->buffer, &info, GST_MAP_READ);

  kms_image_overlay_initialize_images (imageoverlay, frame);
  imageoverlay->priv->cvImage->imageData = (char *) info.data;

  GST_OBJECT_LOCK (imageoverlay);
  /* check if the buffer has metadata */
  metadata = kms_buffer_get_serializable_meta (frame->buffer);

  if (metadata == NULL) {
    goto end;
  }

  faces_list = get_faces (metadata->data);

  if (faces_list != NULL) {
    kms_image_overlay_display_detections_overlay_img (imageoverlay, faces_list);
    g_slist_free_full (faces_list, cvrect_free);
  }

end:
  GST_OBJECT_UNLOCK (imageoverlay);

  gst_buffer_unmap (frame->buffer, &info);

  return GST_FLOW_OK;
}

static void
kms_image_overlay_dispose (GObject * object)
{
  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (kms_image_overlay_parent_class)->dispose (object);
}

static void
kms_image_overlay_finalize (GObject * object)
{
  KmsImageOverlay *imageoverlay = KMS_IMAGE_OVERLAY (object);

  if (imageoverlay->priv->cvImage != NULL)
    cvReleaseImage (&imageoverlay->priv->cvImage);

  G_OBJECT_CLASS (kms_image_overlay_parent_class)->finalize (object);
}

static void
kms_image_overlay_init (KmsImageOverlay * imageoverlay)
{
  imageoverlay->priv = KMS_IMAGE_OVERLAY_GET_PRIVATE (imageoverlay);

  imageoverlay->priv->show_debug_info = FALSE;
  imageoverlay->priv->cvImage = NULL;
}

static void
kms_image_overlay_class_init (KmsImageOverlayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstVideoFilterClass *video_filter_class = GST_VIDEO_FILTER_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, PLUGIN_NAME, 0, PLUGIN_NAME);

  GST_DEBUG ("class init");

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          gst_caps_from_string (VIDEO_SRC_CAPS)));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          gst_caps_from_string (VIDEO_SINK_CAPS)));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "image overlay element", "Video/Filter",
      "Set a defined image in a defined position",
      "David Fernandez <d.fernandezlop@gmail.com>");

  gobject_class->set_property = kms_image_overlay_set_property;
  gobject_class->get_property = kms_image_overlay_get_property;
  gobject_class->dispose = kms_image_overlay_dispose;
  gobject_class->finalize = kms_image_overlay_finalize;

  video_filter_class->transform_frame_ip =
      GST_DEBUG_FUNCPTR (kms_image_overlay_transform_frame_ip);

  /* Properties initialization */
  g_object_class_install_property (gobject_class, PROP_SHOW_DEBUG_INFO,
      g_param_spec_boolean ("show-debug-region", "show debug region",
          "show evaluation regions over the image", FALSE, G_PARAM_READWRITE));

  g_type_class_add_private (klass, sizeof (KmsImageOverlayPrivate));
}

gboolean
kms_image_overlay_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_IMAGE_OVERLAY);
}