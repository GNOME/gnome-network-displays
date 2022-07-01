#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <gst/rtsp-server/rtsp-client.h>
#pragma GCC diagnostic pop

G_BEGIN_DECLS

#define CC_TYPE_CLIENT (cc_client_get_type ())
#define CC_CLIENT_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), CC_TYPE_CLIENT, CCClientClass))

G_DECLARE_FINAL_TYPE (CCClient, cc_client, CC, CLIENT, GstRTSPClient)

CCClient * cc_client_new (void);
void cc_client_query_support (CCClient *self);
void cc_client_trigger_method (CCClient   *self,
                                const gchar *method);


G_END_DECLS
